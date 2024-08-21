#include "compiler_internal.h"
#include "c3_llvm.h"

#if PLATFORM_POSIX
#include <glob.h>
#endif

const char *quote_arg = "\"";
const char *concat_arg = ":";
const char *concat_quote_arg = "+";
const char *quote_concat_arg = "++";
#define add_quote_arg(arg_) vec_add(*args_ref, quote_arg); vec_add(*args_ref, (arg_))
#define add_plain_arg(arg_) vec_add(*args_ref, (arg_))
#define add_concat_arg(arg_, arg2_) vec_add(*args_ref, concat_arg); vec_add(*args_ref, (arg_)); vec_add(*args_ref, (arg2_))
#define add_concat_quote_arg(arg_, arg2_) vec_add(*args_ref, concat_quote_arg); vec_add(*args_ref, (arg_)); vec_add(*args_ref, (arg2_))
#define add_quote_concat_arg(arg_, arg2_) vec_add(*args_ref, quote_concat_arg); vec_add(*args_ref, (arg_)); vec_add(*args_ref, (arg2_))

static char *assemble_linker_command(const char **args, bool extra_quote);
static unsigned assemble_link_arguments(const char **arguments, unsigned len);

static inline bool is_no_pie(RelocModel reloc)
{
	return reloc == RELOC_NONE;
}
static inline bool is_pie(RelocModel reloc)
{
	return reloc == RELOC_BIG_PIE || reloc == RELOC_SMALL_PIE;
}

static const char *ld_target(ArchType arch_type)
{
	switch (arch_type)
	{
		case ARCH_TYPE_X86_64:
			return "elf_x86_64";
		case ARCH_TYPE_X86:
			return "elf_i386";
		case ARCH_TYPE_AARCH64:
			return "aarch64elf";
		case ARCH_TYPE_RISCV32:
			return "elf32lriscv";
		case ARCH_TYPE_RISCV64:
			return "elf64lriscv";
		default:
			error_exit("Architecture currently not available for cross linking.");
	}

}



static void linker_setup_windows(const char ***args_ref, Linker linker_type)
{
	add_plain_arg(compiler.build.win.use_win_subsystem ? "/SUBSYSTEM:WINDOWS" : "/SUBSYSTEM:CONSOLE");
	if (link_libc()) linking_add_link(&compiler.linking, "dbghelp");
	if (linker_type == LINKER_CC) return;
	//add_arg("/MACHINE:X64");
	bool is_debug = false;
	switch (compiler.build.debug_info)
	{
		case DEBUG_INFO_NOT_SET:
			break;
		case DEBUG_INFO_NONE:
			add_plain_arg("/DEBUG:NONE");
			break;
		case DEBUG_INFO_LINE_TABLES:
		case DEBUG_INFO_FULL:
			add_plain_arg("/DEBUG:FULL");
			is_debug = true;
			break;
		default:
			UNREACHABLE
	}
	if (!link_libc()) return;
	bool link_with_dynamic_debug_libc = true;
#if !PLATFORM_WINDOWS
	// The debug version of libc is usually not available on target machines,
	// so we do not link with debug dll versions of libc.
	link_with_dynamic_debug_libc = false;
#endif
	WinCrtLinking linking = compiler.build.win.crt_linking;
	FOREACH(Library *, library, compiler.build.library_list)
	{
		WinCrtLinking wincrt = library->target_used->win_crt;
		if (wincrt == WIN_CRT_DEFAULT || wincrt == linking) continue;
		if (linking != WIN_CRT_DEFAULT)
		{
			WARNING("Mismatch between CRT linking in library %s and previously selected type.", library->dir);
			continue;
		}
		linking = wincrt;
	}

	if (!compiler.build.win.sdk)
	{
		const char *path = windows_cross_compile_library();
		if (path)
		{
			switch (compiler.platform.arch)
			{
				case ARCH_TYPE_ARM:
					scratch_buffer_append("/arm");
					break;
				case ARCH_TYPE_AARCH64:
					scratch_buffer_append("/arm64");
					break;
				case ARCH_TYPE_X86_64:
					scratch_buffer_append("/x64");
					break;
				case ARCH_TYPE_X86:
					scratch_buffer_append("/x86");
					break;
				default:
					UNREACHABLE
			}
			if (file_exists(scratch_buffer_to_string()))
			{
				compiler.build.win.sdk = scratch_buffer_copy();
				// If we only use the msvc cross compile on windows, we
				// avoid linking with dynamic debug dlls.
				link_with_dynamic_debug_libc = false;
			}
		}
	}
	if (compiler.build.win.def)
	{
		add_concat_quote_arg("/def:", compiler.build.win.def);
	}
	if (compiler.build.win.sdk)
	{
		add_concat_quote_arg("/LIBPATH:", compiler.build.win.sdk);
	}
	else
	{
		WindowsSDK *windows_sdk = windows_get_sdk();
		if (!windows_sdk) error_exit("Windows applications cannot be cross compiled without --winsdk.");

		if (!file_is_dir(windows_sdk->vs_library_path)) error_exit("Failed to find windows sdk.");

		add_concat_quote_arg("/LIBPATH:", windows_sdk->windows_sdk_um_library_path);
		add_concat_quote_arg("/LIBPATH:", windows_sdk->windows_sdk_ucrt_library_path);
		add_concat_quote_arg("/LIBPATH:", windows_sdk->vs_library_path);
	}
	linking_add_link(&compiler.linking, "kernel32");
	linking_add_link(&compiler.linking, "ntdll");
	linking_add_link(&compiler.linking, "user32");
	linking_add_link(&compiler.linking, "shell32");
	linking_add_link(&compiler.linking, "Shlwapi");
	linking_add_link(&compiler.linking, "Ws2_32");
	linking_add_link(&compiler.linking, "legacy_stdio_definitions");

	// Do not link any.
	if (compiler.build.win.crt_linking == WIN_CRT_NONE) return;

	if (compiler.build.win.crt_linking == WIN_CRT_STATIC)
	{
		if (is_debug)
		{
			linking_add_link(&compiler.linking, "libucrtd");
			linking_add_link(&compiler.linking, "libvcruntimed");
			linking_add_link(&compiler.linking, "libcmtd");
			linking_add_link(&compiler.linking, "libcpmtd");
		}
		else
		{
			linking_add_link(&compiler.linking, "libucrt");
			linking_add_link(&compiler.linking, "libvcruntime");
			linking_add_link(&compiler.linking, "libcmt");
			linking_add_link(&compiler.linking, "libcpmt");
		}
	}
	else
	{
		// When cross compiling we might not have the relevant debug libraries.
		// if so, then exclude them.
		if (is_debug && link_with_dynamic_debug_libc)
		{
			linking_add_link(&compiler.linking, "ucrtd");
			linking_add_link(&compiler.linking, "vcruntimed");
			linking_add_link(&compiler.linking, "msvcrtd");
			linking_add_link(&compiler.linking, "msvcprtd");
		}
		else
		{
			linking_add_link(&compiler.linking, "ucrt");
			linking_add_link(&compiler.linking, "vcruntime");
			linking_add_link(&compiler.linking, "msvcrt");
			linking_add_link(&compiler.linking, "msvcprt");
		}
	}
	add_plain_arg("/NOLOGO");
}

static void linker_setup_macos(const char ***args_ref, Linker linker_type)
{
	if (linker_type == LINKER_CC)
	{
		add_plain_arg("-target");
		add_plain_arg(compiler.platform.target_triple);
		return;
	}
	add_plain_arg("-arch");
	add_plain_arg(arch_to_linker_arch(compiler.platform.arch));
	if (strip_unused() && compiler.build.type == TARGET_TYPE_EXECUTABLE)
	{
		add_plain_arg("-no_exported_symbols");
		add_plain_arg("-dead_strip");
	}

	// Skip if no libc.
	if (!link_libc()) return;

	if (!compiler.build.macos.sdk)
	{
		error_exit("Cannot crosslink MacOS without providing --macossdk.");
	}
	linking_add_link(&compiler.linking, "System");
	linking_add_link(&compiler.linking, "m");
	add_plain_arg("-syslibroot");
	add_quote_arg(compiler.build.macos.sysroot);
	if (is_no_pie(compiler.platform.reloc_model)) add_plain_arg("-no_pie");
	if (is_pie(compiler.platform.reloc_model)) add_plain_arg("-pie");
	add_plain_arg("-platform_version");
	add_plain_arg("macos");
	if (compiler.build.macos.min_version)
	{
		add_plain_arg(compiler.build.macos.min_version);
	}
	else
	{
		add_plain_arg(str_printf("%d.%d.0", compiler.build.macos.sdk->macos_min_deploy_target.major, compiler.build.macos.sdk->macos_min_deploy_target.minor));
	}
	if (compiler.build.macos.sdk_version)
	{
		add_plain_arg(compiler.build.macos.sdk_version);
	}
	else
	{
		add_plain_arg(str_printf("%d.%d", compiler.build.macos.sdk->macos_deploy_target.major, compiler.build.macos.sdk->macos_deploy_target.minor));
	}
}


static const char *find_freebsd_crt(void)
{
	if (file_exists("/usr/lib/crt1.o"))
	{
		return "/usr/lib/";
	}
	return NULL;
}

static const char *find_arch_glob_path(const char *glob_path, int file_len)
{
#if PLATFORM_POSIX
	glob_t globbuf;
	if (!glob(glob_path, 0, NULL, &globbuf))
	{
		for (int i = 0; i < globbuf.gl_pathc; i++)
		{
			const char *path = globbuf.gl_pathv[i];
			// Avoid qemu problems
			if (compiler.platform.arch != ARCH_TYPE_RISCV64
			    && compiler.platform.arch != ARCH_TYPE_RISCV32
			    && strstr(path, "riscv")) continue;
			size_t len = strlen(path);
			assert(len > file_len);
			const char *res = str_copy(path, len - file_len);
			globfree(&globbuf);
			return res;
		}
		globfree(&globbuf);
	}
#endif
	return NULL;
}
static const char *find_linux_crt(void)
{
	if (compiler.build.linuxpaths.crt) return compiler.build.linuxpaths.crt;
	const char *path = find_arch_glob_path("/usr/lib/*/crt1.o", 6);
	if (!path)
	{
		INFO_LOG("No crt in /usr/lib/*/");
		return NULL;
	}
	INFO_LOG("Found crt at %s", path);
	return path;
}

static const char *find_linux_crt_begin(void)
{
	if (compiler.build.linuxpaths.crtbegin) return compiler.build.linuxpaths.crtbegin;
	const char *path = find_arch_glob_path("/usr/lib/gcc/*/*/crtbegin.o", 10);
	if (!path)
	{
		INFO_LOG("No crtbegin in /usr/lib/gcc/*/*/");
		return NULL;
	}
	INFO_LOG("Found crtbegin at %s", path);
	return path;
}

static void linker_setup_linux(const char ***args_ref, Linker linker_type)
{
	linking_add_link(&compiler.linking, "dl");
	if (linker_type == LINKER_CC)
	{
		if (!link_libc())
		{
			add_plain_arg("-nostdlib");
			return;
		}
		else
		{
			linking_add_link(&compiler.linking, "m");
		}
		if (compiler.build.debug_info == DEBUG_INFO_FULL)
		{
			add_plain_arg("-rdynamic");
		}
		add_plain_arg("-pthread");
		return;
	}
	if (compiler.build.debug_info == DEBUG_INFO_FULL)
	{
		add_plain_arg("-export-dynamic");
	}
	if (is_no_pie(compiler.platform.reloc_model)) add_plain_arg("-no-pie");
	if (is_pie(compiler.platform.reloc_model)) add_plain_arg("-pie");
	if (compiler.platform.arch == ARCH_TYPE_X86_64) add_plain_arg("--eh-frame-hdr");
	if (!link_libc()) return;
	const char *crt_begin_dir = find_linux_crt_begin();
	const char *crt_dir = find_linux_crt();

	if (strip_unused() && compiler.build.type == TARGET_TYPE_EXECUTABLE)
	{
		add_plain_arg("--gc-sections");
	}

	if (!crt_begin_dir || !crt_dir)
	{
		error_exit("Failed to find the C runtime at link time.");
	}
	if (is_pie_pic(compiler.platform.reloc_model))
	{
		add_plain_arg("-pie");
		add_quote_concat_arg(crt_dir, "Scrt1.o");
		add_quote_concat_arg(crt_begin_dir, "crtbeginS.o");
		add_quote_concat_arg(crt_dir, "crti.o");
		add_quote_concat_arg(crt_begin_dir, "crtendS.o");
	}
	else
	{
		add_quote_concat_arg(crt_dir, "crt1.o");
		add_quote_concat_arg(crt_begin_dir, "crtbegin.o");
		add_quote_concat_arg(crt_dir, "crti.o");
		add_quote_concat_arg(crt_begin_dir, "crtend.o");
	}
	add_quote_concat_arg(crt_dir, "crtn.o");
	add_concat_quote_arg("-L", crt_dir);
	add_plain_arg("-L/usr/lib/x86_64-linux-gnu/libdl.so");
	add_plain_arg("--dynamic-linker=/lib64/ld-linux-x86-64.so.2");
	linking_add_link(&compiler.linking, "m");
	linking_add_link(&compiler.linking, "pthread");
	linking_add_link(&compiler.linking, "c");
	add_plain_arg("-L/usr/lib/");
	add_plain_arg("-L/lib/");
	add_plain_arg("-m");
	add_plain_arg(ld_target(compiler.platform.arch));
}

static void linker_setup_freebsd(const char ***args_ref, Linker linker_type)
{
	if (linker_type == LINKER_CC) return;
	if (is_no_pie(compiler.platform.reloc_model)) add_plain_arg("-no-pie");
	if (is_pie(compiler.platform.reloc_model)) add_plain_arg("-pie");
	if (compiler.platform.arch == ARCH_TYPE_X86_64) add_plain_arg("--eh-frame-hdr");

	if (!link_libc()) return;

	const char *crt_dir = find_freebsd_crt();
	if (!crt_dir)
	{
		error_exit("Failed to find the C runtime at link time.");
	}
	if (strip_unused() && compiler.build.type == TARGET_TYPE_EXECUTABLE)
	{
		add_plain_arg("--gc-sections");
	}

	if (is_pie_pic(compiler.platform.reloc_model))
	{
		add_plain_arg("-pie");
		add_quote_concat_arg(crt_dir, "Scrt1.o");
		add_quote_concat_arg(crt_dir, "crtbeginS.o");
		add_quote_concat_arg(crt_dir, "crti.o");
		add_quote_concat_arg(crt_dir, "crtendS.o");
	}
	else
	{
		add_quote_concat_arg(crt_dir, "crt1.o");
		add_quote_concat_arg(crt_dir, "crtbegin.o");
		add_quote_concat_arg(crt_dir, "crti.o");
		add_quote_concat_arg(crt_dir, "crtend.o");
	}
	add_quote_concat_arg(crt_dir, "crtn.o");
	add_concat_quote_arg("-L", crt_dir);
	add_plain_arg("--dynamic-linker=/libexec/ld-elf.so.1");
	linking_add_link(&compiler.linking, "c");
	linking_add_link(&compiler.linking, "m");
	linking_add_link(&compiler.linking, "gcc");
	linking_add_link(&compiler.linking, "gcc_s");

	add_plain_arg("-L/usr/lib/");
	add_plain_arg("-m");
	add_plain_arg(ld_target(compiler.platform.arch));
}

static void add_linked_libs(const char ***args_ref, const char **libs, bool is_win)
{
	FOREACH(const char *, lib, libs)
	{
		INFO_LOG("Linking %s", lib);
		const char *framework = str_remove_suffix(lib, ".framework");
		if (framework)
		{
			add_plain_arg("-framework");
			add_plain_arg(framework);
			continue;
		}
		if (is_win)
		{
			if (str_has_suffix(lib, ".lib"))
			{
				add_plain_arg(lib);
			}
			else
			{
				add_concat_arg(lib, ".lib");
			}
		}
		else
		{
			if (str_has_suffix(lib, ".a") || str_has_suffix(lib, ".so") ||
			    str_has_suffix(lib, ".dylib") || str_has_suffix(lib, ".tbd"))
			{
				add_plain_arg(lib);
			}
			else
			{
				add_concat_quote_arg("-l", lib);
			}
		}
	}
}

static bool linker_setup(const char ***args_ref, const char **files_to_link, unsigned file_count,
                         const char *output_file, Linker linker_type, Linking *linking)
{
	bool is_dylib = compiler.build.type == TARGET_TYPE_DYNAMIC_LIB;
	bool use_win = linker_type == LINKER_LINK_EXE;
	if (!use_win)
	{
		add_plain_arg("-o");
		add_quote_arg(output_file);
	}
	switch (linker_type)
	{
		case LINKER_UNKNOWN:
			break;
		case LINKER_WASM:
			if (!is_dylib && compiler.build.no_entry) add_plain_arg("--no-entry");
			break;
		case LINKER_LD64:
			if (is_dylib) add_plain_arg("-dylib");
			break;
		case LINKER_LD:
			if (is_dylib) add_plain_arg("-shared");
			break;
		case LINKER_LINK_EXE:
			add_concat_quote_arg("/OUT:", output_file);
			if (is_dylib)
			{
				add_plain_arg("/DLL");
			}
			else
			{
				if (compiler.build.no_entry) add_plain_arg("/NOENTRY");
			}
		case LINKER_CC:
			break;
		default:
			UNREACHABLE
	}
	const char *lib_path_opt = use_win ? "/LIBPATH:" : "-L";

	switch (compiler.platform.os)
	{
		case OS_UNSUPPORTED:
			UNREACHABLE
		case OS_TYPE_WIN32:
			linker_setup_windows(args_ref, linker_type);
			break;
		case OS_TYPE_MACOSX:
			linker_setup_macos(args_ref, linker_type);
			break;
		case OS_TYPE_WATCHOS:
		case OS_TYPE_IOS:
		case OS_TYPE_OPENBSD:
		case OS_TYPE_NETBSD:
		case OS_TYPE_TVOS:
		case OS_TYPE_WASI:
			break;
		case OS_TYPE_FREE_BSD:
			linker_setup_freebsd(args_ref, linker_type);
			break;
		case OS_TYPE_LINUX:
			linker_setup_linux(args_ref, linker_type);
			break;
		case OS_TYPE_UNKNOWN:
			if (link_libc())
			{
				error_exit("Linking is not supported for unknown OS.");
			}
			break;
		case OS_TYPE_NONE:
			break;
	}
	for (unsigned i = 0; i < file_count; i++)
	{
		add_quote_arg(files_to_link[i]);
	}

	FOREACH(const char *, dir, compiler.build.linker_libdirs)
	{
		add_quote_concat_arg(lib_path_opt, dir);
	}
	FOREACH(const char *, arg, compiler.build.link_args)
	{
		add_plain_arg(arg);
	}
	add_linked_libs(args_ref, compiler.build.linker_libs, use_win);
	FOREACH(Library *, library, compiler.build.library_list)
	{
		LibraryTarget *target = library->target_used;
		FOREACH(const char *, flag, target->link_flags) add_plain_arg(flag);
		add_linked_libs(args_ref, target->linked_libs, use_win);
	}
	add_linked_libs(args_ref, linking->links, use_win);
	return true;
}
#undef add_arg2
#undef add_arg

static void append_fpie_pic_options(RelocModel reloc, const char ***args_ref)
{
	switch (reloc)
	{
		case RELOC_DEFAULT:
			UNREACHABLE
		case RELOC_NONE:
			add_plain_arg("-fno-pic");
			add_plain_arg("-fno-pie");
			add_plain_arg("-fno-PIC");
			add_plain_arg("-fno-PIE");
			break;
		case RELOC_SMALL_PIC:
			add_plain_arg("-fpic");
			break;
		case RELOC_BIG_PIC:
			add_plain_arg("-fPIC");
			break;
		case RELOC_SMALL_PIE:
			add_plain_arg("-fpie");
			add_plain_arg("-fpic");
			break;
		case RELOC_BIG_PIE:
			add_plain_arg("-fPIE");
			add_plain_arg("-fPIC");
			break;
	}
}

Linker linker_find_linker_type(void)
{
	if (arch_is_wasm(compiler.platform.arch)) return LINKER_WASM;
	switch (compiler.platform.os)
	{
		case OS_UNSUPPORTED:
		case OS_TYPE_UNKNOWN:
		case OS_TYPE_NONE:
		case OS_TYPE_FREE_BSD:
		case OS_TYPE_LINUX:
		case OS_TYPE_NETBSD:
		case OS_TYPE_OPENBSD:
			return LINKER_LD;
		case OS_DARWIN_TYPES:
			return LINKER_LD64;
		case OS_TYPE_WIN32:
			return LINKER_LINK_EXE;
		case OS_TYPE_WASI:
			return LINKER_WASM;
	}
	UNREACHABLE
}

static unsigned assemble_link_arguments(const char **arguments, unsigned len)
{
	unsigned count = 0;
	for (unsigned i = 0; i < len ; i++)
	{
		const char *arg = arguments[i];
		if (arg == quote_arg) continue;
		if (arg == concat_arg || arg == quote_concat_arg || arg == concat_quote_arg)
		{
			const char *a = arguments[++i];
			const char *b = arguments[++i];
			arguments[count++] = str_cat(a, b);
			continue;
		}
		if (count != i)
		{
			arguments[count] = arg;
		}
		count++;
	}
	if (compiler.build.print_linking)
	{
		for (int i = 0; i < count - 1; i++) printf("%s ", arguments[i]);
		puts(arguments[count - 1]);
	}
	return count;
}

static bool link_exe(const char *output_file, const char **files_to_link, unsigned file_count)
{
	INFO_LOG("Using linker directly.");
	const char **args = NULL;
	Linker linker_type = linker_find_linker_type();
	linker_setup(&args, files_to_link, file_count, output_file, linker_type, &compiler.linking);

	const char *error = NULL;
	// This isn't used in most cases, but its contents should get freed after linking.

	bool success;
	unsigned count = assemble_link_arguments(args, vec_size(args));
	switch (compiler.platform.object_format)
	{
		case OBJ_FORMAT_COFF:
			success = llvm_link_coff(args, count, &error);
			break;
		case OBJ_FORMAT_ELF:
			success = llvm_link_elf(args, count, &error);
			break;
		case OBJ_FORMAT_MACHO:
			success = llvm_link_macho(args, count, &error);
			break;
		case OBJ_FORMAT_WASM:
			success = llvm_link_wasm(args, count, &error);
			break;
		default:
			UNREACHABLE
	}
	if (!success)
	{
		error_exit("Failed to create an executable: %s", error);
	}
	INFO_LOG("Linking complete.");
	return true;
}

bool obj_format_linking_supported(ObjectFormatType format_type)
{
	switch (format_type)
	{
		case OBJ_FORMAT_XCOFF:
		case OBJ_FORMAT_AOUT:
		case OBJ_FORMAT_GOFF:
		case OBJ_FORMAT_UNSUPPORTED:
			return false;
		case OBJ_FORMAT_COFF:
		case OBJ_FORMAT_ELF:
		case OBJ_FORMAT_MACHO:
		case OBJ_FORMAT_WASM:
			return true;
	}
	UNREACHABLE

}

static char *assemble_linker_command(const char **args, bool extra_quote)
{
	scratch_buffer_clear();
	if (extra_quote) scratch_buffer_append_char('"');
	unsigned count = vec_size(args);
	for (unsigned i = 0; i < count; i++)
	{
		if (i != 0) scratch_buffer_append_char(' ');
		const char *arg = args[i];
		assert(arg != scratch_buffer.str && "Incorrectly passed a scratch buffer string as an argument.");
		if (arg == quote_arg)
		{
			scratch_buffer_append_char('"');
			scratch_buffer_append_in_quote(args[++i]);
			scratch_buffer_append_char('"');
			continue;
		}
		if (arg == concat_arg)
		{
			scratch_buffer_append(args[++i]);
			scratch_buffer_append(args[++i]);
			continue;
		}
		if (arg == quote_concat_arg)
		{
			scratch_buffer_append_char('"');
			scratch_buffer_append_in_quote(args[++i]);
			scratch_buffer_append_in_quote(args[++i]);
			scratch_buffer_append_char('"');
			continue;
		}
		if (arg == concat_quote_arg)
		{
			scratch_buffer_append(args[++i]);
			scratch_buffer_append_char('"');
			scratch_buffer_append_in_quote(args[++i]);
			scratch_buffer_append_char('"');
			continue;
		}
		scratch_buffer_append(arg);
	}
	if (extra_quote) scratch_buffer_append_char('"');
	return scratch_buffer_to_string();
}


void platform_linker(const char *output_file, const char **files, unsigned file_count)
{
	const char **parts = NULL;
	const char ***args_ref = &parts;
	Linker linker_type = LINKER_CC;
	if (compiler.build.linker_type == LINKER_TYPE_CUSTOM)
	{
		INFO_LOG("Using linker %s.", compiler.build.custom_linker_path);
		add_quote_arg(compiler.build.custom_linker_path);
		switch (compiler.platform.object_format)
		{
			case OBJ_FORMAT_UNSUPPORTED:
			case OBJ_FORMAT_GOFF:
			case OBJ_FORMAT_XCOFF:
			case OBJ_FORMAT_AOUT:
				linker_type = LINKER_UNKNOWN;
				break;
			case OBJ_FORMAT_COFF:
				linker_type = LINKER_LINK_EXE;
				break;
			case OBJ_FORMAT_ELF:
				linker_type = LINKER_LD;
				break;
			case OBJ_FORMAT_MACHO:
				linker_type = LINKER_LD64;
				break;
			case OBJ_FORMAT_WASM:
				linker_type = LINKER_WASM;
				break;
		}
	}
	else
	{
		INFO_LOG("Using cc linker.");
		vec_add(parts, compiler.build.cc ? compiler.build.cc : "cc");
		append_fpie_pic_options(compiler.platform.reloc_model, &parts);
	}

	linker_setup(&parts, files, file_count, output_file, linker_type, &compiler.linking);
	const char *output = assemble_linker_command(parts, PLATFORM_WINDOWS);
	if (compiler.build.print_linking) puts(output);
	if (system(output) != 0)
	{
		error_exit("Failed to link executable '%s' using command '%s'.\n", output_file, output);
	}
	if (os_is_apple(compiler.platform.os) && compiler.build.debug_info == DEBUG_INFO_FULL)
	{
		// Create .dSYM
		scratch_buffer_clear();
		scratch_buffer_printf("dsymutil -arch %s \"%s\"", arch_to_linker_arch(compiler.platform.arch), output_file);
		if (compiler.build.print_linking) puts(scratch_buffer_to_string());
		if (system(scratch_buffer_to_string()) != 0)
		{
			puts("Failed to create .dSYM files, debugging will be impacted.");
		}
	}
	printf("Program linked to executable '%s'.\n", output_file);
}

const char *cc_compiler(const char *cc, const char *file, const char *flags, const char **include_dirs, const char *output_subdir)
{
	const char *dir = compiler.build.object_file_dir;
	if (!dir) dir = compiler.build.build_dir;
	if (output_subdir) dir = file_append_path(dir, output_subdir);
	dir_make(dir);
	bool is_cl_exe = str_eq(cc, "cl.exe");
	char *filename = NULL;
	bool split_worked = file_namesplit(file, &filename, NULL);
	if (!split_worked) error_exit("Cannot compile '%s'", file);
	size_t len = strlen(filename);
	// Remove .cpp or .c
	if (len > 5 && memcmp(filename + len - 4, ".cpp", 4) == 0)
	{
		len -= 4;
		filename[len] = 0;
	}
	else if (len > 2 && memcmp(filename + len - 2, ".c", 2) == 0)
	{
		len -= 2;
		filename[len] = 0;
	}
	const char *out_name = dir
	                      ? str_printf("%s/%s%s", dir, filename, get_object_extension())
	                      : str_printf("%s%s", filename, get_object_extension());;
	const char **parts = NULL;
	const char ***args_ref = &parts;
	add_quote_arg(cc);

	FOREACH(const char *, include_dir, include_dirs)
	{
		add_concat_quote_arg(is_cl_exe ? "/I" : "-I ", include_dir);
	}

	const bool pie_set =
			flags != NULL &&
			(strstr(flags, "-fno-PIE") || // This is a weird case, but probably don't set PIE if
			 strstr(flags, "-fno-pie") || // it is being set in user defined cflags.
			 strstr(flags, "-fpie") ||
			 strstr(flags, "-fPIE")); // strcasestr is apparently nonstandard >:(
	if (!pie_set && !is_cl_exe)
	{
		append_fpie_pic_options(compiler.platform.reloc_model, &parts);
	}

	add_plain_arg(is_cl_exe ? "/c" : "-c");
	if (flags) add_plain_arg(flags);
	add_quote_arg(file);
	if (is_cl_exe)
	{
		add_concat_quote_arg("/Fo:", out_name);
	}
	else
	{
		add_plain_arg("-o");
		add_quote_arg(out_name);
	}

	const char *output = assemble_linker_command(parts, PLATFORM_WINDOWS);
	DEBUG_LOG("Compiling c sources using '%s'", output);
	if (system(output) != 0)
	{
		error_exit("Failed to compile c sources using command '%s'.\n", output);
	}
	return out_name;
}

bool dynamic_lib_linker(const char *output_file, const char **files, unsigned file_count)
{
	INFO_LOG("Using linker directly.");
	const char **args = NULL;
	if (compiler.build.linker_type == LINKER_TYPE_CUSTOM) vec_add(args, compiler.build.custom_linker_path);
	Linker linker_type = linker_find_linker_type();
	linker_setup(&args, files, file_count, output_file, linker_type, &compiler.linking);

	if (compiler.build.linker_type == LINKER_TYPE_CUSTOM)
	{
		const char *command = assemble_linker_command(args, PLATFORM_WINDOWS);
		if (compiler.build.print_linking) puts(command);
		DEBUG_LOG("Linker arguments: %s to %d", command, compiler.platform.object_format);
		if (system(command) != 0)
		{
			error_exit("Failed to create a dynamic library using command '%s'.", command);
		}
		return true;
	}
	bool success;
	const char *error = NULL;
	unsigned count = assemble_link_arguments(args, vec_size(args));
	switch (compiler.platform.object_format)
	{
		case OBJ_FORMAT_COFF:
			success = llvm_link_coff(args, count, &error);
			break;
		case OBJ_FORMAT_ELF:
			success = llvm_link_elf(args, count, &error);
			break;
		case OBJ_FORMAT_MACHO:
			success = llvm_link_macho(args, count, &error);
			break;
		case OBJ_FORMAT_WASM:
			success = llvm_link_wasm(args, count, &error);
			break;
		default:
			UNREACHABLE
	}
	if (!success)
	{
		error_exit("Failed to create a dynamic library: %s", error);
	}
	INFO_LOG("Linking complete.");
	return true;
}

bool static_lib_linker(const char *output_file, const char **files, unsigned file_count)
{
	ArFormat format;
	switch (compiler.platform.os)
	{
		case OS_DARWIN_TYPES:
			format = AR_DARWIN;
			break;
		case OS_TYPE_WIN32:
			format = AR_COFF;
			break;
		case OS_TYPE_FREE_BSD:
		case OS_TYPE_NETBSD:
		case OS_TYPE_OPENBSD:
			format = AR_BSD;
			break;
		case OS_TYPE_LINUX:
		default:
			format = AR_GNU;
			break;
	}
	return llvm_ar(output_file, files, file_count, format);
}

bool linker(const char *output_file, const char **files, unsigned file_count)
{
	return link_exe(output_file, files, file_count);
}

/**
 * From Clang
 * .Cases("aarch64elf", "aarch64linux", {ELF64LEKind, EM_AARCH64})
		  .Cases("aarch64elfb", "aarch64linuxb", {ELF64BEKind, EM_AARCH64})
		  .Cases("armelf", "armelf_linux_eabi", {ELF32LEKind, EM_ARM})
		  .Case("elf32_x86_64", {ELF32LEKind, EM_X86_64})
		  .Cases("elf32btsmip", "elf32btsmipn32", {ELF32BEKind, EM_MIPS})
		  .Cases("elf32ltsmip", "elf32ltsmipn32", {ELF32LEKind, EM_MIPS})
		  .Case("elf32lriscv", {ELF32LEKind, EM_RISCV})
		  .Cases("elf32ppc", "elf32ppclinux", {ELF32BEKind, EM_PPC})
		  .Cases("elf32lppc", "elf32lppclinux", {ELF32LEKind, EM_PPC})
		  .Case("elf64btsmip", {ELF64BEKind, EM_MIPS})
		  .Case("elf64ltsmip", {ELF64LEKind, EM_MIPS})
		  .Case("elf64lriscv", {ELF64LEKind, EM_RISCV})
		  .Case("elf64ppc", {ELF64BEKind, EM_PPC64})
		  .Case("elf64lppc", {ELF64LEKind, EM_PPC64})
		  .Cases("elf_amd64", "elf_x86_64", {ELF64LEKind, EM_X86_64})
		  .Case("elf_i386", {ELF32LEKind, EM_386})
		  .Case("elf_iamcu", {ELF32LEKind, EM_IAMCU})
		  .Case("elf64_sparc", {ELF64BEKind, EM_SPARCV9})
		  .Case("msp430elf", {ELF32LEKind, EM_MSP430})
 */
