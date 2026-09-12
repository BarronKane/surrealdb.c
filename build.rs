extern crate cbindgen;

use std::env;
use std::fs;
use std::path::Path;
use std::process::Command;

/// Environment variable that opts in to driving the CMake build from cargo.
///
/// Off by default: CMake already invokes cargo, so making this the default
/// would create a reentrant loop and would drag cmake, a C compiler and
/// network access into every `cargo build`.
const DRIVE_CMAKE: &str = "SURREALDB_C_DRIVE_CMAKE";

fn main() {
    let crate_dir = env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR not set");
    let out_dir = env::var("OUT_DIR").expect("OUT_DIR not set");

    // Emitting *any* rerun-if-changed replaces cargo's default whole-package
    // watch, so every input the header depends on has to be listed here. In
    // particular `src` must be present: without it cbindgen does not re-run
    // when the Rust API changes and the committed header silently goes stale.
    println!("cargo:rerun-if-changed=src");
    println!("cargo:rerun-if-changed=cbindgen.toml");
    println!("cargo:rerun-if-env-changed={DRIVE_CMAKE}");

    // OUT_DIR is the canonical location for generated output.
    let generated = Path::new(&out_dir).join("surrealdb.h");
    cbindgen::generate(&crate_dir)
        .expect("unable to generate bindings")
        .write_to_file(&generated);

    // Let dependent build scripts find the generated header.
    println!("cargo:include={out_dir}");

    // Mirror it into include/ so that a plain `cargo build` leaves a usable
    // header in the tree and consumers can go straight to `#include`.
    let tracked = Path::new(&crate_dir).join("include").join("surrealdb.h");
    sync_header(&generated, &tracked);

    if env::var_os(DRIVE_CMAKE).is_some() {
        drive_cmake(&crate_dir);
    }
}

/// Copy the generated header over the tracked one, best effort.
///
/// Skips the write when the contents already match, which keeps the working
/// tree clean and makes concurrent builds for multiple targets benign — they
/// would write identical bytes. A read-only source tree (vendored, Nix,
/// distro packaging) is reported as a warning rather than failing the build:
/// the authoritative copy is the one in OUT_DIR.
fn sync_header(generated: &Path, tracked: &Path) {
    let new = match fs::read(generated) {
        Ok(bytes) => bytes,
        Err(e) => {
            println!("cargo:warning=could not read generated header: {e}");
            return;
        }
    };

    if fs::read(tracked).is_ok_and(|current| current == new) {
        return;
    }

    if let Some(parent) = tracked.parent() {
        if let Err(e) = fs::create_dir_all(parent) {
            println!("cargo:warning=could not create {}: {e}", parent.display());
            return;
        }
    }

    if let Err(e) = fs::write(tracked, &new) {
        println!(
            "cargo:warning=could not update {}: {e} \
             (generated header is available at {})",
            tracked.display(),
            generated.display()
        );
    }
}

/// Configure and build the CMake project.
///
/// Only reached when the caller opts in. The child process has the opt-in
/// variable removed from its environment so that the cargo build CMake starts
/// does not recurse back into here.
fn drive_cmake(crate_dir: &str) {
    let build_dir = Path::new(crate_dir).join("build");

    for args in [
        vec!["-S", crate_dir, "-B", build_dir.to_str().unwrap()],
        vec!["--build", build_dir.to_str().unwrap()],
    ] {
        match Command::new("cmake")
            .args(&args)
            .env_remove(DRIVE_CMAKE)
            .status()
        {
            Ok(status) if status.success() => {}
            Ok(status) => {
                println!("cargo:warning=cmake {} failed: {status}", args.join(" "));
                return;
            }
            Err(e) => {
                println!("cargo:warning=could not run cmake: {e}");
                return;
            }
        }
    }
}
