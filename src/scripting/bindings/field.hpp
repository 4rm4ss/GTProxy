#pragma once
#include <type_traits>

#include <sol/sol.hpp>

namespace scripting::bindings {
namespace detail {
template <typename>
struct member_object_traits;

template <typename C, typename T>
struct member_object_traits<T C::*> {
    using class_type = C;
    using value_type = T;
};
}

// Usertype member for `"name", field<&Type::member>()`.
//
// sol2 3.5.0 cannot compile plain data-member bindings (`"name", &Type::member`)
// with Clang 18+ ("address of overloaded function 'call' does not match
// required type"), which also affects macOS. With Clang, data members become an
// equivalent read/write property: scalars by value (some live in packed
// structs, which cannot be referenced), everything else by reference so nested
// objects stay editable from Lua. Other compilers, and member functions, get
// the member pointer unchanged.
template <auto Member>
[[nodiscard]] constexpr auto field()
{
#if defined(__clang__)
    if constexpr (std::is_member_object_pointer_v<decltype(Member)>) {
        using Traits = detail::member_object_traits<decltype(Member)>;
        using C = typename Traits::class_type;
        using T = typename Traits::value_type;

        if constexpr (std::is_scalar_v<T>) {
            return sol::property(
                [](const C& self) -> T { return self.*Member; },
                [](C& self, const T value) { self.*Member = value; }
            );
        } else if constexpr (std::is_array_v<T> || !std::is_copy_assignable_v<T>) {
            return sol::readonly_property([](C& self) -> T& { return self.*Member; });
        } else {
            return sol::property(
                [](C& self) -> T& { return self.*Member; },
                [](C& self, const T& value) { self.*Member = value; }
            );
        }
    } else {
        return Member;
    }
#else
    return Member;
#endif
}
}
