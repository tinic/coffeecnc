#ifndef COFFEECNC_VIEWER_BOOST_HPP
#define COFFEECNC_VIEWER_BOOST_HPP

// Boost.Beast 1.83 uses std::aligned_storage in this header. Clang warns at
// instantiation even when Boost is a system include. Scope it to that header.
#if defined(__clang__) && __has_include(<boost/beast/core/detail/type_traits.hpp>)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <boost/beast/core/detail/type_traits.hpp>
#pragma clang diagnostic pop
#endif

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#endif
