#define NOB_IMPLEMENTATION
#include "nob.h"
#include <stdlib.h>
#include <stdbool.h>

#define STR2(x) #x
#define STR(x) STR2(x)

#define ANDROID_MIN_SDK    26
#define ANDROID_TARGET_SDK 35

#define APP_AUTHOR     "raylib"
#define APP_NAME       "raymob"
#define APP_LABEL_NAME "raymob"

static char* home = NULL;
static char* ndk_path = NULL;
static char* ndk_toolchain_path = NULL;
static char* sdk_path = NULL;
static char* android_build_tools = NULL;
static char* native_app_glue_path = NULL;
static char* java_home = NULL;

typedef struct {
    Pipe *items;
    size_t count;
    size_t capacity;
} Pipes;

void flush_pipe(Pipe pipe, Fd outfd) {
    static char buf[4096];
    ssize_t n;
    while ((n = read(pipe.read, buf, sizeof(buf))) > 0) {
        write(outfd, buf, n);
    }
}

void flush_pipes(Pipes *pipes, Fd outfd) {
    for (size_t i = 0; i < pipes->count; i++) {
        flush_pipe(pipes->items[i], outfd);
    }
    pipes->count = 0;
}

const char *sources[] = {
    "main.c",
};

const char* java_bin(const char *tool) {
    if (java_home) {
        return temp_sprintf("%s/bin/%s", java_home, tool);
    } else {
        return tool;
    }
}

void cc(Cmd *cmd) {
    cmd_append(cmd, temp_sprintf("%s/bin/clang", ndk_toolchain_path));
}

void target_flags(Cmd *cmd) {
    cmd_append(cmd, "-mfix-cortex-a53-835769");
    cmd_append(cmd, temp_sprintf("--target=aarch64-linux-android%d", ANDROID_MIN_SDK));
    cmd_append(cmd, temp_sprintf("--sysroot=%s/sysroot", ndk_toolchain_path));
}
void ldflags(Cmd *cmd) {
    cmd_append(cmd, "-Wl,-soname,libmain.so");
    cmd_append(cmd, "-Wl,--exclude-libs,libatomic.a"); // not sure why this is needed, but it was in the raylib Makefile.Android
    cmd_append(cmd, "-Wl,--build-id"); // likewise
    cmd_append(cmd, "-Wl,--no-undefined");
    cmd_append(cmd, "-Wl,-z,noexecstack");
    cmd_append(cmd, "-Wl,-z,relro");
    cmd_append(cmd, "-Wl,-z,now");
    cmd_append(cmd, "-Wl,--gc-sections");
    cmd_append(cmd, "-Wl,--warn-shared-textrel");
    cmd_append(cmd, "-Wl,--fatal-warnings");
    cmd_append(cmd, "-Wl,--wrap=fopen");
    cmd_append(cmd, "-u","ANativeActivity_onCreate");
    cmd_append(cmd, "-L./build/");
    cmd_append(cmd, "-L./build/lib");
}

void includes(Cmd *cmd) {
    cmd_append(cmd, temp_sprintf("-I%s", native_app_glue_path));
    cmd_append(cmd, "-I./deps/raylib-6.0/src");
    cmd_append(cmd, "-I./deps/raymob/");
}

void cflags(Cmd *cmd) {
    if (isatty(STDERR_FILENO))
        cmd_append(cmd, "-fcolor-diagnostics");
    cmd_append(cmd, "-Wall");
    cmd_append(cmd, "-Wformat");
    cmd_append(cmd, "-Werror=format-security");
    cmd_append(cmd, "-std=c99");
    cmd_append(cmd, "-D_GNU_SOURCE");
    cmd_append(cmd, "-DGRAPHICS_API_OPENGL_ES2");
    cmd_append(cmd, "-ffunction-sections");
    cmd_append(cmd, "-funwind-tables");
    cmd_append(cmd, "-fstack-protector-strong");
    cmd_append(cmd, "-fPIC");
    cmd_append(cmd, "-no-canonical-prefixes");
    cmd_append(cmd, "-DANDROID");
    cmd_append(cmd, "-DPLATFORM_ANDROID");
    target_flags(cmd);
}

bool create_temp_project_dirs() {
    if (!mkdir_if_not_exists("build")) return false;
    if (!mkdir_if_not_exists("build/lib")) return false;
    if (!mkdir_if_not_exists("build/lib/arm64-v8a")) return false;
    if (!mkdir_if_not_exists("build/bin")) return false;
    if (!mkdir_if_not_exists("build/compiled")) return false;
    return true;
}

// bool copy_project_resources() {
//     nob_log(NOB_INFO, "TODO: copy_project_resources\n");
//     return false;
// }

// bool generate_loader_script() {
//     nob_log(NOB_INFO, "TODO: generate_loader_script\n");
//     return false;
// }
// bool generate_android_manifest() {
//     nob_log(NOB_INFO, "TODO: generate_android_manifest\n");
//     return false;
// }
bool generate_apk_keystore(Cmd *cmd) {
    const char *keystore = "build/"APP_NAME".keystore";
    if (!file_exists(keystore)) {
        cmd_append(cmd, java_bin("keytool"));
        cmd_append(cmd, "-genkeypair");
        cmd_append(cmd, "-validity", "10000");
        cmd_append(cmd, "-dname", "CN="APP_AUTHOR",O=Android,C=ES");
        cmd_append(cmd, "-keystore", keystore);
        cmd_append(cmd, "-storepass", "foobar");
        cmd_append(cmd, "-alias", APP_NAME"Key");
        cmd_append(cmd, "-keyalg", "RSA");
        if (!cmd_run(cmd)) return false;
    }
    return true;
}

bool collect_regular_files(Walk_Entry entry) {
    File_Paths *files = entry.data;
    if (entry.type == FILE_REGULAR) {
        da_append(files, temp_strdup(entry.path));
    }
    return true;
}

bool config_project_package(Cmd *cmd, Procs *procs) {
    // this step generates a starting APK with resources + R.java for loading
    // outputs: R.java, resources.apk
    bool result = true;
    size_t checkpoint = temp_save();
    const char *apk_path = "build/bin/resources.apk";
    File_Paths files = {0};

    // collect all res and asset files that get bundled into the resources.apk
    walk_dir("./res", collect_regular_files, .data = &files);
    walk_dir("./assets", collect_regular_files, .data = &files);
    // also include the AndroidManifest.xml
    da_append(&files, "AndroidManifest.xml");
    // if any of them are newer than output APK, rebuild
    if (needs_rebuild(apk_path, files.items, files.count)) {
        nob_log(NOB_INFO, "assets files have changed, rebuilding %s", apk_path);
        /**** aapt2 compile step ****/
        // res files
        files.count = 0;
        walk_dir("./res", collect_regular_files, .data = &files);
        for (size_t i = 0; i < files.count; i++) {
            cmd_append(cmd, temp_sprintf("%s/aapt2", android_build_tools));
            cmd_append(cmd, "compile");
            cmd_append(cmd, files.items[i]);
            cmd_append(cmd, "-o", "build/compiled");
            if (!cmd_run(cmd, .async = procs)) return_defer(false);
        }
        if (!procs_flush(procs)) return_defer(false);

        /**** aapt2 link step ****/
        // grab the generated *.flat files form the compile step
        files.count = 0;
        walk_dir("./build/compiled", collect_regular_files, .data = &files);
        cmd_append(cmd, temp_sprintf("%s/aapt2", android_build_tools));
        cmd_append(cmd, "link");
        cmd_append(cmd, "--min-sdk-version", STR(ANDROID_MIN_SDK));
        cmd_append(cmd, "--target-sdk-version", STR(ANDROID_TARGET_SDK));
        cmd_append(cmd, "-o", apk_path);
        cmd_append(cmd, "--java", "build/gen/");
        cmd_append(cmd, "-I", temp_sprintf("%s/platforms/android-%d/android.jar", sdk_path, ANDROID_TARGET_SDK));
        da_append_many(cmd, files.items, files.count);
        cmd_append(cmd, "--manifest", "AndroidManifest.xml");
        cmd_append(cmd, "-A", "assets");
        // cmd_append(cmd, "-v");
        if (!cmd_run(cmd)) return_defer(false);
    }
defer:
    temp_rewind(checkpoint);
    da_free(files);
    return result;
}

const char *objname(const char *srcname) {
    String_View sv = sv_from_cstr(srcname);
    String_View name = {0};
    while (sv.count > 0) {
        name = sv_chop_by_delim(&sv, '/');
    }
    name = sv_chop_by_delim(&name, '.');
    return temp_sprintf(SV_Fmt".o", SV_Arg(name));
}

static const char *raylib_sources[] = {
    "./deps/raylib-6.0/src/rcore.c",
    "./deps/raylib-6.0/src/rshapes.c",
    "./deps/raylib-6.0/src/rtextures.c",
    "./deps/raylib-6.0/src/rtext.c",
    "./deps/raylib-6.0/src/rmodels.c",
    "./deps/raylib-6.0/src/raudio.c",
};

bool build_raylib(Cmd *cmd, Procs *procs, Pipes *pipes) {
    if (!needs_rebuild("build/lib/libraylib.a", raylib_sources, ARRAY_LEN(raylib_sources))) return true;
    nob_log(NOB_INFO, "Rebuilding raylib");
    bool result = true;
    size_t checkpoint = temp_save();
    // build objects
    for (size_t i = 0; i < ARRAY_LEN(raylib_sources); i++) {
        cc(cmd);
        cmd_append(cmd, "-c", raylib_sources[i]);
        cmd_append(cmd, "-o", temp_sprintf("build/%s", objname(raylib_sources[i])));
        cflags(cmd);
        includes(cmd);
        Pipe pipe = {0};
        if (!pipe_create(&pipe)) return 1;
        da_append(pipes, pipe);
        if (!cmd_run(cmd, .async = procs, .stderr_fd = pipe.write)) break;
    }
    bool success = procs_flush(procs);
    flush_pipes(pipes, STDERR_FILENO);
    if (!success) return_defer(false);

    // link
    cmd_append(cmd, temp_sprintf("%s/bin/llvm-ar", ndk_toolchain_path));
    cmd_append(cmd, "rcs");
    cmd_append(cmd, "build/lib/libraylib.a");
    for (size_t i = 0; i < ARRAY_LEN(raylib_sources); i++) {
        cmd_append(cmd, temp_sprintf("build/%s", objname(raylib_sources[i])));
    }
    if (!cmd_run(cmd)) return_defer(false);
defer:
    temp_rewind(checkpoint);
    return result;
}

bool compile_objs(Cmd *cmd, Procs *procs, Pipes *pipes) {
    bool result = true;
    size_t checkpoint = temp_save();
    const char *native_app_glue_src = temp_sprintf("%s/android_native_app_glue.c", native_app_glue_path);
    // native app glue
    if (needs_rebuild1("build/android_native_app_glue.o", native_app_glue_src)) {
        nob_log(NOB_INFO, "Rebuilding android_native_app_glue.o");
        cc(cmd);
        cmd_append(cmd, "-c", native_app_glue_src);
        cmd_append(cmd, "-o", "build/android_native_app_glue.o");
        cflags(cmd);
        cmd_append(cmd, temp_sprintf("-I%s", native_app_glue_path));
        if (!cmd_run(cmd)) return_defer(false);
    }

    // raylib
    if (!build_raylib(cmd, procs, pipes)) return_defer(false);

    // main
    if (needs_rebuild1("build/main.o", "main.c")) {
        nob_log(NOB_INFO, "Rebuilding main.o");
        cc(cmd);
        cmd_append(cmd, "-c", "main.c");
        cmd_append(cmd, "-o", "build/main.o");
        cflags(cmd);
        includes(cmd);
        if (!cmd_run(cmd)) return_defer(false);
    }

defer:
    temp_rewind(checkpoint);
    return result;
}

bool compile_project_code(Cmd *cmd) {
	// $(CC) -o $(PROJECT_BUILD_PATH)/lib/$(ANDROID_ARCH_NAME)/lib$(PROJECT_LIBRARY_NAME).so $(OBJS) -shared $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
    const char *so_out = "build/lib/arm64-v8a/libmain.so";
    static const char *so_sources[] = {
        "build/main.o",
        "build/android_native_app_glue.o",
        "build/lib/libraylib.a",
    };
    if (needs_rebuild(so_out, so_sources, ARRAY_LEN(so_sources))) {
        nob_log(NOB_INFO, "Rebuilding libmain.so");
        cc(cmd);
        cmd_append(cmd, "-shared");
        cmd_append(cmd, "-o", so_out);
        cmd_append(cmd, "build/main.o");
        cmd_append(cmd, "build/android_native_app_glue.o");
        target_flags(cmd);
        ldflags(cmd);
        // libs
        cmd_append(cmd, "-lm", "-lc", "-llog", "-ldl");
        cmd_append(cmd, "-lraylib");
        cmd_append(cmd, "-landroid");
        cmd_append(cmd, "-lEGL", "-lGLESv2", "-lOpenSLES");
        if (!cmd_run(cmd)) return false;
    }

    return true;
}
bool compile_project_class(Cmd *cmd) {
    static const char *java_sources[] = {
        "java/com/"APP_AUTHOR"/"APP_NAME"/NativeLoader.java",
        "build/gen/com/"APP_AUTHOR"/"APP_NAME"/R.java",
        // "java/com/"APP_AUTHOR"/"APP_NAME"/Features.java",
        // "java/com/"APP_AUTHOR"/features/SoftKeyboard.java",
        // "java/com/"APP_AUTHOR"/features/DisplayManager.java",
        // "java/com/"APP_AUTHOR"/features/Vibration.java",
        // "java/com/"APP_AUTHOR"/features/Sensor.java",
    };
    if (needs_rebuild("build/com/"APP_AUTHOR"/"APP_NAME"/R.class", java_sources, ARRAY_LEN(java_sources)) ||
        needs_rebuild("build/com/"APP_AUTHOR"/"APP_NAME"/NativeLoader.class", java_sources, ARRAY_LEN(java_sources))) {
        nob_log(NOB_INFO, "Java files have changed, rebuilding classes");
        cmd_append(cmd, java_bin("javac"));
        cmd_append(cmd, "--release", "11");
        cmd_append(cmd, "-d", "build/");
        cmd_append(cmd, "-classpath", temp_sprintf("%s/platforms/android-%d/android.jar", sdk_path, ANDROID_TARGET_SDK));
        da_append_many(cmd, java_sources, ARRAY_LEN(java_sources));
        return cmd_run(cmd);
    }
    return true;
}

bool find_class_files(Walk_Entry entry) {
    String_View path = sv_from_cstr(entry.path);
    File_Paths *class_files = entry.data;
    if (sv_ends_with(path, sv_from_cstr(".class"))) {
        da_append(class_files, temp_strdup(entry.path));
    }
    return true;
}

bool compile_project_class_dex(Cmd *cmd) {
    bool result = true;
    size_t checkpoint = temp_save();
    File_Paths class_files = {0};
    walk_dir("build/com", find_class_files, .data = &class_files);
    if (needs_rebuild("./build/bin/classes.dex", class_files.items, class_files.count)) {
        nob_log(NOB_INFO, "Java classes have changed, rebuilding classes.dex");
        cmd_append(cmd, temp_sprintf("%s/d8", android_build_tools));
        cmd_append(cmd, "--output", "./build/bin");
        cmd_append(cmd, "--lib", temp_sprintf("%s/platforms/android-%d/android.jar", sdk_path, ANDROID_TARGET_SDK));
        cmd_append(cmd, "--min-api", STR(ANDROID_MIN_SDK));
        for (size_t i = 0; i < class_files.count; i++) {
            cmd_append(cmd, class_files.items[i]);
        }
        if (!cmd_run(cmd)) return_defer(false);
    }
defer:
    temp_rewind(checkpoint);
    da_free(class_files);
    return result;
}

bool create_project_apk_package(Cmd *cmd) {
	// $(ANDROID_BUILD_TOOLS)/aapt package -f -M $(PROJECT_BUILD_PATH)/AndroidManifest.xml -S $(PROJECT_BUILD_PATH)/res -A $(PROJECT_BUILD_PATH)/assets -I $(ANDROID_HOME)/platforms/android-$(ANDROID_API_VERSION)/android.jar -F $(PROJECT_BUILD_PATH)/bin/$(PROJECT_NAME).unsigned.apk $(PROJECT_BUILD_PATH)/bin
	// cd $(PROJECT_BUILD_PATH) && $(ANDROID_BUILD_TOOLS)/aapt add bin/$(PROJECT_NAME).unsigned.apk lib/$(ANDROID_ARCH_NAME)/lib$(PROJECT_LIBRARY_NAME).so $(PROJECT_SHARED_LIBS)
    bool result = true;
    size_t checkpoint = temp_save();
    const char *pwd_save = get_current_dir_temp();
    const char *apk_out = APP_NAME".unsigned.apk";
    set_current_dir("build");
    const char *sources[] = {
        "bin/resources.apk",
        "bin/classes.dex",
        "lib/arm64-v8a/libmain.so",
    };
    if (needs_rebuild(apk_out, sources, ARRAY_LEN(sources))) {
        // copy over the resources.apk as a starting point
        if (!copy_file("bin/resources.apk", APP_NAME".unsigned.apk")) return false;
        // throw in the dex file
        cmd_append(cmd, "zip");
        cmd_append(cmd, "-j");                            // junk paths (add to root of zip)
        cmd_append(cmd, APP_NAME".unsigned.apk"); // zip archive
        cmd_append(cmd, "bin/classes.dex");         // file to add
        if (!cmd_run(cmd)) return_defer(false);
        // add app shared library
        cmd_append(cmd, "zip");
        cmd_append(cmd, "-0"); // don't deflate: required for .so so it can be mmap'd
        cmd_append(cmd, APP_NAME".unsigned.apk");
        cmd_append(cmd, "lib/arm64-v8a/libmain.so");
        if (!cmd_run(cmd)) return_defer(false);
    }
defer:
    set_current_dir(pwd_save);
    temp_rewind(checkpoint);
    return result;
}

bool zipalign_project_apk_package(Cmd *cmd) {
    size_t checkpoint = temp_save();
    bool result = true;
    const char *apk_in  = "build/"APP_NAME".unsigned.apk";
    const char *apk_out = "build/"APP_NAME".aligned.apk";
    if (needs_rebuild1(apk_out, apk_in)) {
        cmd_append(cmd, temp_sprintf("%s/zipalign", android_build_tools));
        cmd_append(cmd, "-P", "16");  // enforce 16K alignment for .so files
        cmd_append(cmd, "-f");        // force overwrite existing output file
        cmd_append(cmd, "-v");        // verbose output
        cmd_append(cmd, "4");         // 4 byte alignment for regular entries
        cmd_append(cmd, apk_in);
        cmd_append(cmd, apk_out);
        if (!cmd_run(cmd)) return_defer(false);
    }
defer:
    temp_rewind(checkpoint);
    return result;
}

bool sign_project_apk_package(Cmd *cmd) {
    const char *keystore = "build/"APP_NAME".keystore";
    const char *apk_in = "build/"APP_NAME".aligned.apk";
    const char *apk_out = "build/"APP_NAME".apk";
    bool result = true;
    size_t checkpoint = temp_save();
    if (needs_rebuild1(apk_out, apk_in)) {
        cmd_append(cmd, temp_sprintf("%s/apksigner", android_build_tools));
        cmd_append(cmd, "-J-enable-native-access=ALL-UNNAMED"); // be rid of pesky warning message
        cmd_append(cmd, "sign");
        cmd_append(cmd, "--in", apk_in);
        cmd_append(cmd, "--out", apk_out);
        cmd_append(cmd, "--verbose");
        cmd_append(cmd, "--ks", keystore);
        cmd_append(cmd, "--ks-pass", "pass:foobar");
        if (!cmd_run(cmd)) return_defer(false);
    }
defer:
    temp_rewind(checkpoint);
    return result;
}

bool build_apk(Cmd *cmd, Procs *procs, Pipes *pipes) {
    if (!create_temp_project_dirs()) return false;
    // In Makefile.Android, this generated NativeLoader.java
    // if (!generate_loader_script()) return false;
    // In Makefile.Android, this generated the AndroidManifest.xml
    // if (!generate_android_manifest()) return false;
    if (!generate_apk_keystore(cmd)) return false;
    if (!config_project_package(cmd, procs)) return false;
    if (!compile_objs(cmd, procs, pipes)) return false;
    if (!compile_project_code(cmd)) return false;
    if (!compile_project_class(cmd)) return false;
    if (!compile_project_class_dex(cmd)) return false;
    if (!create_project_apk_package(cmd)) return false;
    if (!zipalign_project_apk_package(cmd)) return false;
    if (!sign_project_apk_package(cmd)) return false;

    return true;
}

bool setup_paths() {
    home = getenv("HOME");
    if (!home) {
        nob_log(NOB_ERROR, "no HOME in environment, something has gone horribly wrong!\n");
        return false;
    }
    ndk_path = getenv("ANDROID_NDK_ROOT");
    if (!ndk_path) {
        nob_log(NOB_ERROR, "ANDROID_NDK_ROOT must be set to the NDK install location");
        nob_log(NOB_ERROR, "You can download the NDK toolchain at https://developer.android.com/ndk/downloads\n");
        return false;
    }
    if (!file_exists(ndk_path)) {
        nob_log(NOB_ERROR, "Could not find NDK path: %s", ndk_path);
        nob_log(NOB_ERROR, "You can download the NDK toolchain at https://developer.android.com/ndk/downloads\n");
        return false;
    }
    nob_log(NOB_INFO, "Found Android NDK at: %s", ndk_path);

    ndk_toolchain_path = temp_sprintf("%s/toolchains/llvm/prebuilt/linux-x86_64", ndk_path);
    if (!file_exists(ndk_path)) {
        nob_log(NOB_ERROR, "Could not find toolchain in provided NDK path: %s", ndk_path);
        nob_log(NOB_ERROR, "Your NDK installation may be broken");
        return false;
    }
    native_app_glue_path = temp_sprintf("%s/sources/android/native_app_glue", ndk_path);
    if (!file_exists(ndk_path)) {
        nob_log(NOB_ERROR, "Could not find native_app_glue in provided NDK path: %s", ndk_path);
        nob_log(NOB_ERROR, "Your NDK installation may be broken");
        return false;
    }
    sdk_path = getenv("ANDROID_HOME");
    if (!sdk_path) {
        nob_log(NOB_ERROR, "ANDROID_HOME must be set to the SDK install location");
        nob_log(NOB_ERROR, "If you need to install the SDK, look for the Android `sdkmanager` tool.");
        return false;
    }
    if (!file_exists(sdk_path)) {
        nob_log(NOB_ERROR, "Could not find Android SDK path at %s", sdk_path);
        nob_log(NOB_ERROR, "If you need to install the SDK, look for the Android `sdkmanager` tool.");
        return false;
    }
    nob_log(NOB_INFO, "Found Android SDK at: %s", sdk_path);
    android_build_tools = getenv("ANDROID_BUILD_TOOLS");
    if (!android_build_tools) {
        nob_log(NOB_ERROR, "ANDROID_BUILD_TOOLS must be set");
        return false;
    }
    java_home = getenv("JAVA_HOME");
    if (java_home) {
        nob_log(NOB_INFO, "Using Java installation in `%s`", java_home);
    }
    return true;
}

bool install_apk(Cmd *cmd) {
    bool result = true;
    size_t checkpoint = temp_save();

	// $(ANDROID_PLATFORM_TOOLS)/adb install $(PROJECT_NAME).apk
    cmd_append(cmd, temp_sprintf("%s/platform-tools/adb", sdk_path));
    cmd_append(cmd, "install");
    cmd_append(cmd, "build/"APP_NAME".apk");
    if (!cmd_run(cmd)) return_defer(false);
defer:
    temp_rewind(checkpoint);
    return result;
}

void usage(const char *prog, FILE *out) {
    fprintf(out, "%s [build|install|deploy]\n", prog);
    fprintf(out, "  -h,--help   print this help\n");
    fprintf(out, "  build       build APK [default when no arg provided]\n");
    fprintf(out, "  install     build and install APK to connected device\n");
    fprintf(out, "  deploy      like `install`, but also opens logcat for debugging\n");
}

typedef struct {
    const char **items;
    size_t count;
    size_t capacity;
} Arg_List;

typedef struct {
    bool help;
    Arg_List rest;
} Args;

bool parse_args(Args *args, char **argv, int argc) {
    while (argc) {
        String_View arg = sv_from_cstr(*argv);
        if (*arg.data == '-') {
            while (arg.count && *arg.data == '-') {
                arg.data += 1;
                arg.count -= 1;
            }
            if (sv_eq(arg, sv_from_cstr("h")) || sv_eq(arg, sv_from_cstr("help"))) {
                args->help = true;
            } else {
                nob_log(NOB_ERROR, "Unrecognized flag: %s\n", *argv);
                return false;
            }
        } else {
            da_append(&args->rest, *argv);
        }
        argc--;
        argv++;
    }
    return true;
}

int main(int argc, char *argv[]) {
    NOB_GO_REBUILD_URSELF(argc, argv);
    Cmd cmd = {0};
    Procs procs = {0};
    Pipes pipes = {0};
    const char *prog = shift(argv, argc);
    Args args = {0};
    if (!parse_args(&args, argv, argc)) {
        usage(prog, stderr);
        return 1;
    }
    if (!setup_paths()) return 1;
    if (args.help) {
        usage(prog, stdout);
        return 0;
    }
    if (args.rest.count == 0) {
        // just do the build
        if (!build_apk(&cmd, &procs, &pipes)) return 1;
    } else {
        const char *arg = shift(args.rest.items, args.rest.count);
        if (strcmp(arg, "build") == 0) {
            if (!build_apk(&cmd, &procs, &pipes)) return 1;
        } else if (strcmp(arg, "install") == 0) {
            if (!build_apk(&cmd, &procs, &pipes)) return 1;
            if (!install_apk(&cmd)) return 1;
        } else if (strcmp(arg, "deploy") == 0) {
            if (!build_apk(&cmd, &procs, &pipes)) return 1;
            if (!install_apk(&cmd)) return 1;
            cmd_append(&cmd, temp_sprintf("%s/platform-tools/adb", sdk_path));
            cmd_append(&cmd, "logcat", "-c");
            if (!cmd_run(&cmd)) return 1;
            cmd_append(&cmd, temp_sprintf("%s/platform-tools/adb", sdk_path));
            cmd_append(&cmd, "logcat");
            cmd_append(&cmd, "raylib:V");
            cmd_append(&cmd, "*:S");
            if (!cmd_run(&cmd)) return 1;
        } else {
            fprintf(stderr, "Unrecognized command: %s\n", arg);
            usage(prog, stderr);
            return 1;
        }
    }

    return 0;
}
