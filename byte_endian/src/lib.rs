/**
 * @file lib.rs
 * @author Krisna Pranav
 * @brief pty
 * @version 3.0
 * @date 2024-09-28
 *
 * @copyright Copyright (c) 2022-2024 vultureOS Developers, Krisna Pranav
 *
 */

#![no_std]
#![warn(clippy::pedantic)]

macro_rules! impl_traits {
    ($($endian_type:ident),+) => {
        $(
            impl Endian<$endian_type> for $endian_type {
                fn to_be(&self) -> $endian_type {
                    <$endian_type>::to_be(*self)
                }

                fn to_le(&self) -> $endian_type {
                    <$endian_type>::to_le(*self)
                }

                fn from_be(value: $endian_type) -> $endian_type {
                    <$endian_type>::from_be(value)
                }

                fn from_le(value: $endian_type) -> $endian_type {
                    <$endian_type>::from_le(value)
                }
            }
        )+

        impl_traits!(@make_impl $($endian_type),+ => LittleEndian);
        impl_traits!(@make_impl $($endian_type),+ => BigEndian);
    };

    (@make_impl $($endian_type:ident),+ => $type:ident) => {
        impl<T: Endian<T>> From<T> for $type<T> {
            #[inline]
            fn from(value: T) -> Self {
                Self::new(value)
            }
        }

        $(
            impl From<$type<$endian_type>> for $endian_type {
                #[inline]
                fn from(value: $type<$endian_type>) -> Self {
                    value.to_native()
                }
            }
        )*
    };
}