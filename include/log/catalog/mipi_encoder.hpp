#pragma once

#include <conc/concurrency.hpp>
#include <log/catalog/catalog.hpp>
#include <log/log.hpp>

#include <stdx/bit.hpp>
#include <stdx/compiler.hpp>
#include <stdx/ct_string.hpp>
#include <stdx/tuple.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <utility>

namespace {
template <logging::level L, typename S, typename T>
constexpr auto to_message() {
    constexpr auto s = S::value;
    using char_t = typename std::remove_cv_t<decltype(s)>::value_type;
    return [&]<template <typename...> typename Tuple, typename... Args,
               std::size_t... Is>(Tuple<Args...> const &,
                                  std::integer_sequence<std::size_t, Is...>) {
        return sc::message<
            L, sc::undefined<sc::args<Args...>, char_t, s[Is]...>>{};
    }(T{}, std::make_integer_sequence<std::size_t, std::size(s)>{});
}

template <logging::level L, typename Msg> constexpr auto to_message(Msg msg) {
    if constexpr (requires { msg.args; }) {
        return to_message<L, decltype(msg.str), decltype(msg.args)>();
    } else {
        return to_message<L, Msg, stdx::tuple<>>();
    }
}

template <typename S> constexpr auto to_module() {
    constexpr auto s = S::value;
    using char_t = typename std::remove_cv_t<decltype(s)>::value_type;
    return [&]<std::size_t... Is>(std::integer_sequence<std::size_t, Is...>) {
        return sc::module_string<sc::undefined<void, char_t, s[Is]...>>{};
    }(std::make_integer_sequence<std::size_t, std::size(s)>{});
}
} // namespace

namespace logging::mipi {
template <typename TDestinations> struct log_handler {
    constexpr explicit log_handler(TDestinations &&ds) : dests{std::move(ds)} {}

    template <logging::level Level, typename ModuleId,
              typename FilenameStringType, typename LineNumberType,
              typename MsgType>
    ALWAYS_INLINE auto log(FilenameStringType, LineNumberType,
                           MsgType const &msg) -> void {
        log_msg<Level, ModuleId>(msg);
    }

    template <logging::level Level, typename ModuleId, typename Msg>
    ALWAYS_INLINE auto log_msg(Msg msg) -> void {
        msg.apply([&]<typename StringType>(StringType, auto... args) {
            using Message = decltype(to_message<Level>(msg));
            using Module = decltype(to_module<ModuleId>());
            dispatch_message<Level>(catalog<Message>(), module<Module>(),
                                    static_cast<std::uint32_t>(args)...);
        });
    }

    template <auto Version, stdx::ct_string S = ""> auto log_build() -> void {
        if constexpr (S.empty() and stdx::bit_width(Version) <= 22) {
            dispatch_pass_by_args(((Version & 0x3'00'00'0u) << 10u) |
                                  ((Version & 0xff'ff'fu) << 4u));
        } else if constexpr (S.empty() and stdx::bit_width(Version) <= 54) {
            constexpr auto subtype = 0x1u; // compact64
            dispatch_pass_by_args(
                static_cast<std::uint32_t>(((Version & 0x3'00'00'0u) << 10u) |
                                           ((Version & 0xff'ff'fu) << 4u) |
                                           (subtype << 24u)),
                static_cast<std::uint32_t>((Version >> 22u) & 0xffff'ffffu));
        } else {
            constexpr std::uint32_t subtype = 0x2u; // long
            constexpr std::uint32_t opt_len = 0x1u << 9u;
            constexpr std::uint32_t len = S.size() + sizeof(std::uint64_t);
            constexpr std::uint32_t byte_len = sizeof(std::uint32_t) + 2u + len;
            constexpr std::uint32_t dword_len =
                (byte_len + sizeof(std::uint32_t) - 1) / sizeof(std::uint32_t);

            std::array<std::uint32_t, dword_len> args{};
            args[0] = (subtype << 24u) | opt_len;
            args[1] = stdx::to_le(len);
            auto const ver = stdx::to_le(static_cast<std::uint64_t>(Version));
            auto dest = stdx::bit_cast<std::uint8_t *>(&args[1]) + 2;
            dest = std::copy_n(stdx::bit_cast<std::uint8_t const *>(&ver),
                               sizeof(std::uint64_t), dest);
            std::copy(std::cbegin(S.value), std::cend(S.value), dest);
            dispatch_pass_by_buffer(args.data(), args.size());
        }
    }

  private:
    constexpr static auto make_catalog32_header(logging::level level,
                                                module_id m) -> std::uint32_t {
        constexpr auto type = 0x3u;    // catalog
        constexpr auto subtype = 0x1u; // id32_p32

        return (subtype << 24u) | (m << 16u) |
               (static_cast<std::uint32_t>(level) << 4u) | type;
    }

    constexpr static auto make_short32_header(string_id id) -> std::uint32_t {
        return (id << 4u) | 1u;
    }

    template <typename... MsgDataTypes>
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
    NEVER_INLINE auto dispatch_pass_by_args(MsgDataTypes &&...msg_data)
        -> void {
        stdx::for_each(
            [&]<typename Dest>(Dest &dest) {
                conc::call_in_critical_section<Dest>([&] {
                    dest.log_by_args(std::forward<MsgDataTypes>(msg_data)...);
                });
            },
            dests);
    }

    NEVER_INLINE auto dispatch_pass_by_buffer(std::uint32_t *msg,
                                              std::uint32_t msg_size) -> void {
        stdx::for_each(
            [&]<typename Dest>(Dest &dest) {
                conc::call_in_critical_section<Dest>(
                    [&] { dest.log_by_buf(msg, msg_size); });
            },
            dests);
    }

    template <logging::level Level, typename... MsgDataTypes>
    ALWAYS_INLINE auto dispatch_message(string_id id,
                                        [[maybe_unused]] module_id m,
                                        MsgDataTypes &&...msg_data) -> void {
        if constexpr (sizeof...(msg_data) == 0u) {
            dispatch_pass_by_args(make_short32_header(id));
        } else if constexpr (sizeof...(msg_data) <= 2u) {
            dispatch_pass_by_args(make_catalog32_header(Level, m), id,
                                  std::forward<MsgDataTypes>(msg_data)...);
        } else {
            std::array args = {make_catalog32_header(Level, m), id,
                               std::forward<MsgDataTypes>(msg_data)...};
            dispatch_pass_by_buffer(args.data(), args.size());
        }
    }

    TDestinations dests;
};

template <typename... TDestinations> struct config {
    using destinations_tuple_t = stdx::tuple<TDestinations...>;
    constexpr explicit config(TDestinations... dests)
        : logger{stdx::tuple{std::move(dests)...}} {}

    log_handler<destinations_tuple_t> logger;
};
template <typename... Ts> config(Ts...) -> config<Ts...>;
} // namespace logging::mipi
