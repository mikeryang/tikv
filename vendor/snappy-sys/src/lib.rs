extern crate libc;

use libc::{c_int, size_t};

#[link(name = "snappy")]
extern {
    pub fn snappy_compress(input: *const u8,
                           input_length: size_t,
                           compressed: *mut u8,
                           compressed_length: *mut size_t) -> c_int;
    pub fn snappy_uncompress(compressed: *const u8,
                             compressed_length: size_t,
                             uncompressed: *mut u8,
                             uncompressed_length: *mut size_t) -> c_int;
    pub fn snappy_max_compressed_length(source_length: size_t) -> size_t;
    pub fn snappy_uncompressed_length(compressed: *const u8,
                                      compressed_length: size_t,
                                      result: *mut size_t) -> c_int;
    pub fn snappy_validate_compressed_buffer(compressed: *const u8,
                                             compressed_length: size_t) -> c_int;
}
