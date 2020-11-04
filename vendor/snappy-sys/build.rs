extern crate pkg_config;
extern crate cmake;

use std::env;
use std::fs;

use cmake::Config;

fn main() {
    let src = env::current_dir().unwrap().join("snappy");
    let dst = Config::new("snappy").build_target("snappy").build();
    let mut build = dst.join("build");
    let stub = build.join("snappy-stubs-public.h");
    if cfg!(target_os = "windows") {
        let profile = match &*env::var("PROFILE").unwrap_or("debug".to_owned()) {
            "bench" | "release" => "Release",
            _ => "Debug",
        };
        build = build.join(profile);
    }
    println!("cargo:root={}", build.display());
    println!("cargo:rustc-link-lib=static=snappy");
    println!("cargo:rustc-link-search=native={}", build.display());
    fs::copy(src.join("snappy.h"), build.join("snappy.h")).unwrap();
    if cfg!(target_os = "windows") {
        fs::copy(stub, build.join("snappy-stubs-public.h")).unwrap();
    }
    configure_stdcpp();
}

fn configure_stdcpp() {
    // From: https://github.com/alexcrichton/cc-rs/blob/master/src/lib.rs
    let target = env::var("TARGET").unwrap();
    let cpp = if target.contains("darwin") {
        Some("c++")
    } else if target.contains("windows") {
        None
    } else {
        Some("stdc++")
    };
    if let Some(cpp) = cpp {
        println!("cargo:rustc-link-lib={}", cpp);
    }
}
