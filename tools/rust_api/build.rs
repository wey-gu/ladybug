use std::env;
use std::path::{Path, PathBuf};

fn link_mode() -> &'static str {
    if env::var("LBUG_SHARED").is_ok() {
        "dylib"
    } else {
        "static"
    }
}

fn get_target() -> String {
    env::var("PROFILE").unwrap()
}

fn link_libraries() {
    // This also needs to be set by any crates using it if they want to use extensions
    if !cfg!(windows) && link_mode() == "static" {
        println!("cargo:rustc-link-arg=-rdynamic");
    }
    if cfg!(windows) && link_mode() == "dylib" {
        println!("cargo:rustc-link-lib=dylib=lbug_shared");
    } else if link_mode() == "dylib" {
        println!("cargo:rustc-link-lib={}=lbug", link_mode());
    } else if rustversion::cfg!(since(1.82)) {
        println!("cargo:rustc-link-lib=static:+whole-archive=lbug");
    } else {
        println!("cargo:rustc-link-lib=static=lbug");
    }
    if link_mode() == "static" {
        if cfg!(windows) {
            println!("cargo:rustc-link-lib=dylib=msvcrt");
            println!("cargo:rustc-link-lib=dylib=shell32");
            println!("cargo:rustc-link-lib=dylib=ole32");
        } else if cfg!(target_os = "macos") {
            println!("cargo:rustc-link-lib=dylib=c++");
        } else {
            println!("cargo:rustc-link-lib=dylib=stdc++");
        }

        for lib in [
            "utf8proc",
            "antlr4_cypher",
            "antlr4_runtime",
            "re2",
            "fastpfor",
            "parquet",
            "thrift",
            "snappy",
            "zstd",
            "miniz",
            "mbedtls",
            "brotlidec",
            "brotlicommon",
            "lz4",
            "roaring_bitmap",
            "simsimd",
            "yyjson",
        ] {
            // These are implementation dependencies of liblbug. Pulling every
            // object from each archive duplicates symbols supplied by host
            // crates, notably simsimd. Only the database and its extensions
            // need whole-archive semantics for registration symbols.
            println!("cargo:rustc-link-lib=static={lib}");
        }

        // Our fork statically links builtin extensions (extension_config.cmake
        // EXTENSION_STATIC_LINK_LIST): the generated_extension_loader in
        // liblbug references their load() symbols, so the archives must be on
        // the link line too (they are staged into OUT_DIR/builtin_extensions
        // by build_bundled_cmake).
        for lib in [
            "lbug_fts_static_extension",
            "lbug_algo_static_extension",
            "lbug_json_static_extension",
            "lbug_vector_static_extension",
            "snowball",
        ] {
            if rustversion::cfg!(since(1.82)) {
                println!("cargo:rustc-link-lib=static:+whole-archive={lib}");
            } else {
                println!("cargo:rustc-link-lib=static={lib}");
            }
        }
    }
}

fn build_bundled_cmake() -> Vec<PathBuf> {
    let lbug_root = {
        let root = Path::new(&std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("lbug-src");
        if root.is_symlink() || root.is_dir() {
            root
        } else {
            // If the path is not directory, this is probably an in-source build on windows where the
            // symlink is unreadable.
            Path::new(&std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("../..")
        }
    };

    let mut build = cmake::Config::new(&lbug_root);
    build
        .no_build_target(true)
        .define("BUILD_SHELL", "OFF")
        .define("BUILD_SINGLE_FILE_HEADER", "OFF")
        .define("AUTO_UPDATE_GRAMMAR", "OFF");
    if cfg!(windows) {
        build.generator("Ninja");
        build.cxxflag("/EHsc");
        build.define("CMAKE_MSVC_RUNTIME_LIBRARY", "MultiThreadedDLL");
        build.define("CMAKE_POLICY_DEFAULT_CMP0091", "NEW");
    }
    if let Ok(jobs) = std::env::var("NUM_JOBS") {
        std::env::set_var("CMAKE_BUILD_PARALLEL_LEVEL", jobs);
    }
    let build_dir = build.build();

    let lbug_lib_path = build_dir.join("build").join("src");
    println!("cargo:rustc-link-search=native={}", lbug_lib_path.display());

    // Stage the statically-linked builtin extension archives (our fork's
    // extension_config.cmake builds them as lib<name>_static.lbug_extension
    // under <source>/extension/<name>/build) into OUT_DIR with standard
    // lib*.a names so rustc-link-lib can find them.
    let ext_lib_dir = build_dir.join("builtin_extensions");
    std::fs::create_dir_all(&ext_lib_dir).expect("create builtin_extensions dir");
    for ext in ["fts", "algo", "json", "vector"] {
        let src = lbug_root
            .join("extension")
            .join(ext)
            .join("build")
            .join(format!("lib{ext}_static.lbug_extension"));
        let dst = ext_lib_dir.join(format!("liblbug_{ext}_static_extension.a"));
        if src.exists() {
            std::fs::copy(&src, &dst)
                .unwrap_or_else(|e| panic!("copy {} -> {}: {e}", src.display(), dst.display()));
        } else {
            panic!(
                "builtin extension archive missing: {} (the fork links {ext} statically; \
                 did the cmake build produce it?)",
                src.display()
            );
        }
    }
    println!("cargo:rustc-link-search=native={}", ext_lib_dir.display());

    // fts depends on the bundled snowball stemmer (built out-of-source).
    let snowball_dir = build_dir
        .join("build")
        .join("extension")
        .join("fts")
        .join("third_party")
        .join("snowball");
    println!("cargo:rustc-link-search=native={}", snowball_dir.display());

    for dir in [
        "utf8proc",
        "antlr4_cypher",
        "antlr4_runtime",
        "re2",
        "brotli",
        "alp",
        "fastpfor",
        "parquet",
        "thrift",
        "snappy",
        "zstd",
        "miniz",
        "mbedtls",
        "lz4",
        "roaring_bitmap",
        "simsimd",
        "yyjson",
    ] {
        let lib_path = build_dir
            .join("build")
            .join("third_party")
            .join(dir)
            .canonicalize()
            .unwrap_or_else(|_| {
                panic!(
                    "Could not find {}/build/third_party/{}",
                    build_dir.display(),
                    dir
                )
            });
        println!("cargo:rustc-link-search=native={}", lib_path.display());
    }

    vec![
        lbug_root.join("src/include"),
        build_dir.join("build/src"),
        build_dir.join("build/src/include"),
        lbug_root.join("third_party/nlohmann_json"),
        lbug_root.join("third_party/fastpfor"),
        lbug_root.join("third_party/alp/include"),
    ]
}

fn build_ffi(
    bridge_file: &str,
    out_name: &str,
    source_file: &str,
    bundled: bool,
    include_paths: &Vec<PathBuf>,
) {
    let mut build = cxx_build::bridge(bridge_file);
    build.file(source_file);

    if bundled {
        build.define("LBUG_BUNDLED", None);
    }
    if get_target() == "debug" || get_target() == "relwithdebinfo" {
        build.define("ENABLE_RUNTIME_CHECKS", "1");
    }
    if link_mode() == "static" {
        build.define("LBUG_STATIC_DEFINE", None);
    }

    build.includes(include_paths);

    println!("cargo:rerun-if-env-changed=LBUG_SHARED");

    println!("cargo:rerun-if-changed=include/lbug_rs.h");
    println!("cargo:rerun-if-changed=src/lbug_rs.cpp");
    // Note that this should match the lbug-src/* entries in the package.include list in Cargo.toml
    // Unfortunately they appear to need to be specified individually since the symlink is
    // considered to be changed each time.
    println!("cargo:rerun-if-changed=lbug-src/src");
    // The builtin extensions (fts/algo/json/vector) are statically linked into this
    // crate, so an edit under extension/ changes the shipped binary. Without this
    // line cargo reports the crate Fresh and silently keeps the stale archive, which
    // is invisible: the build is green and the fix is simply not in the artifact.
    println!("cargo:rerun-if-changed=lbug-src/extension");
    println!("cargo:rerun-if-changed=lbug-src/cmake");
    println!("cargo:rerun-if-changed=lbug-src/third_party");
    println!("cargo:rerun-if-changed=lbug-src/CMakeLists.txt");
    println!("cargo:rerun-if-changed=lbug-src/tools/CMakeLists.txt");

    if cfg!(windows) {
        build.flag("/std:c++20");
        build.flag("/MD");
    } else {
        build.flag("-std=c++2a");
    }
    build.compile(out_name);
}

fn main() {
    if env::var("DOCS_RS").is_ok() {
        // Do nothing; we're just building docs and don't need the C++ library
        return;
    }

    let mut bundled = false;
    let mut include_paths =
        vec![Path::new(&std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("include")];

    if let (Ok(lbug_lib_dir), Ok(lbug_include)) =
        (env::var("LBUG_LIBRARY_DIR"), env::var("LBUG_INCLUDE_DIR"))
    {
        println!("cargo:rustc-link-search=native={lbug_lib_dir}");
        println!("cargo:rustc-link-arg=-Wl,-rpath,{lbug_lib_dir}");
        include_paths.push(Path::new(&lbug_include).to_path_buf());
    } else {
        include_paths.extend(build_bundled_cmake());
        bundled = true;
    }
    if link_mode() == "static" {
        link_libraries();
    }
    build_ffi(
        "src/ffi.rs",
        "lbug_rs",
        "src/lbug_rs.cpp",
        bundled,
        &include_paths,
    );

    if cfg!(feature = "arrow") {
        build_ffi(
            "src/ffi/arrow.rs",
            "lbug_arrow_rs",
            "src/lbug_arrow.cpp",
            bundled,
            &include_paths,
        );
    }
    if link_mode() == "dylib" {
        link_libraries();
    }
}
