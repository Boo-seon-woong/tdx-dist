#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xfb794c10, "__kmalloc_cache_noprof" },
	{ 0xbd03ed67, "vmemmap_base" },
	{ 0xbd03ed67, "page_offset_base" },
	{ 0xd82e1996, "set_memory_encrypted" },
	{ 0x956d4674, "__free_pages" },
	{ 0xcb8b6ec6, "kfree" },
	{ 0x2520ea93, "refcount_warn_saturate" },
	{ 0xd710adbf, "__kmalloc_noprof" },
	{ 0xd82e1996, "set_memory_decrypted" },
	{ 0x047842fc, "alloc_pages_noprof" },
	{ 0xa59da3c0, "down_write" },
	{ 0xa59da3c0, "up_write" },
	{ 0x0688a4fa, "cc_mkdec" },
	{ 0xf68c3f59, "vm_map_pages_zero" },
	{ 0xd272d446, "__fentry__" },
	{ 0x0011321a, "misc_register" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0xab571464, "misc_deregister" },
	{ 0xbd03ed67, "random_kmalloc_seed" },
	{ 0xcbaa3a2c, "kmalloc_caches" },
	{ 0xd268ca91, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0xfb794c10,
	0xbd03ed67,
	0xbd03ed67,
	0xd82e1996,
	0x956d4674,
	0xcb8b6ec6,
	0x2520ea93,
	0xd710adbf,
	0xd82e1996,
	0x047842fc,
	0xa59da3c0,
	0xa59da3c0,
	0x0688a4fa,
	0xf68c3f59,
	0xd272d446,
	0x0011321a,
	0xd272d446,
	0xab571464,
	0xbd03ed67,
	0xcbaa3a2c,
	0xd268ca91,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"__kmalloc_cache_noprof\0"
	"vmemmap_base\0"
	"page_offset_base\0"
	"set_memory_encrypted\0"
	"__free_pages\0"
	"kfree\0"
	"refcount_warn_saturate\0"
	"__kmalloc_noprof\0"
	"set_memory_decrypted\0"
	"alloc_pages_noprof\0"
	"down_write\0"
	"up_write\0"
	"cc_mkdec\0"
	"vm_map_pages_zero\0"
	"__fentry__\0"
	"misc_register\0"
	"__x86_return_thunk\0"
	"misc_deregister\0"
	"random_kmalloc_seed\0"
	"kmalloc_caches\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "D973E594B1C12373F89C192");
