/*
 * Copyright (C) 2013-2021 Canonical, Ltd.
 * Copyright (C) 2022-2023 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-arch.h"

static const stress_help_t help[] = {
	{ NULL,	"remap N",	"start N workers exercising page remappings" },
	{ NULL,	"remap-ops N",	"stop after N remapping bogo operations" },
	{ NULL,	NULL,		NULL }
};

#if defined(HAVE_REMAP_FILE_PAGES) &&	\
    !defined(STRESS_ARCH_SPARC)

#define N_PAGES		(512)

typedef uint16_t stress_mapdata_t;

static inline void *stress_get_umapped_addr(const size_t sz)
{
	void *addr;

	addr = mmap(NULL, sz, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (addr == MAP_FAILED)
		return NULL;

	(void)munmap(addr, sz);
	return addr;
}

/*
 *  check_order()
 *	check page order
 */
static void check_order(
	const stress_args_t *args,
	const size_t stride,
	const stress_mapdata_t *data,
	const size_t *order,
	const char *ordering)
{
	size_t i;
	bool failed;

	for (failed = false, i = 0; i < N_PAGES; i++) {
		if (data[i * stride] != order[i]) {
			failed = true;
			break;
		}
	}
	if (failed)
		pr_fail("%s: remap %s order pages failed\n",
			args->name, ordering);
}

/*
 *  remap_order()
 *	remap based on given order
 */
static int remap_order(
	const stress_args_t *args,
	const size_t stride,
	stress_mapdata_t *data,
	const size_t *order,
	const size_t page_size,
	double *duration,
	double *count)
{
	size_t i;

	for (i = 0; i < N_PAGES; i++) {
		double t;
		int ret;
#if defined(HAVE_MLOCK)
		int lock_ret;

		lock_ret = mlock(data + (i * stride), page_size);
#endif
		t = stress_time_now();
		ret = remap_file_pages(data + (i * stride), page_size,
			0, order[i], 0);
		if (ret == 0) {
			(*duration) += stress_time_now() - t;
			(*count) += 1.0;
		}
#if defined(HAVE_MLOCK)
		if (lock_ret == 0) {
			(void)munlock(data + (i * stride), page_size);
		}
		if (ret) {
			/* mlocked remap failed? try unlocked remap */
			ret = remap_file_pages(data + (i * stride), page_size,
				0, order[i], 0);
		}
#endif
		if (ret < 0) {
			pr_fail("%s: remap_file_pages failed, errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			return -1;
		}
	}

	return 0;
}

/*
 *  stress_remap
 *	stress page remapping
 */
static int stress_remap(const stress_args_t *args)
{
	stress_mapdata_t *data;
	uint8_t *unmapped, *mapped;
	const size_t page_size = args->page_size;
	const size_t data_size = N_PAGES * page_size;
	const size_t stride = page_size / sizeof(*data);
	size_t i, mapped_size = page_size + page_size;
	double duration = 0.0, count = 0.0, rate = 0.0;

	data = mmap(NULL, data_size, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (data == MAP_FAILED) {
		pr_inf_skip("%s: mmap failed to allocate %zd bytes: "
			"errno=%d (%s), skipping stressor\n",
			args->name, data_size, errno, strerror(errno));
		return EXIT_NO_RESOURCE;
	}

	for (i = 0; i < N_PAGES; i++)
		data[i * stride] = (stress_mapdata_t)i;

	unmapped = stress_get_umapped_addr(page_size);
	mapped = mmap(NULL, mapped_size, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (mapped != MAP_FAILED) {
		/*
		 * attempt to unmap last page so we know there
		 * is an unmapped page following the
		 * mapped address space
		 */
		if (munmap(mapped + page_size, page_size) == 0) {
			mapped_size = page_size;
		} else {
			/* failed */
			(void)munmap(mapped, mapped_size);
			mapped_size = 0;
			mapped = NULL;
		}
	} else {
		/* we tried and failed */
		mapped = NULL;
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	do {
		size_t order[N_PAGES];

		/* Reverse pages */
		for (i = 0; i < N_PAGES; i++)
			order[i] = N_PAGES - 1 - i;

		if (remap_order(args, stride, data, order, page_size, &duration, &count) < 0)
			break;
		check_order(args, stride, data, order, "reverse");

		/* random order pages */
		for (i = 0; i < N_PAGES; i++)
			order[i] = i;
		for (i = 0; i < N_PAGES; i++) {
			size_t tmp, j = stress_mwc32modn(N_PAGES);

			tmp = order[i];
			order[i] = order[j];
			order[j] = tmp;
		}

		if (remap_order(args, stride, data, order, page_size, &duration, &count) < 0)
			break;
		check_order(args, stride, data, order, "random");

		/* all mapped to 1 page */
		for (i = 0; i < N_PAGES; i++)
			order[i] = 0;
		if (remap_order(args, stride, data, order, page_size, &duration, &count) < 0)
			break;
		check_order(args, stride, data, order, "all-to-1");

		/* reorder pages back again */
		for (i = 0; i < N_PAGES; i++)
			order[i] = i;
		if (remap_order(args, stride, data, order, page_size, &duration, &count) < 0)
			break;
		check_order(args, stride, data, order, "forward");

		/*
		 *  exercise some illegal remapping calls
		 */
		if (unmapped) {
			VOID_RET(int, remap_file_pages((void *)unmapped, page_size, 0, 0, 0));

			/* Illegal flags */
			VOID_RET(int, remap_file_pages((void *)unmapped, page_size, 0, 0, ~0));

			/* Invalid prot */
			VOID_RET(int, remap_file_pages((void *)unmapped, page_size, ~0, order[0], 0));
		}
		if (mapped) {
			VOID_RET(int, remap_file_pages((void *)(mapped + page_size), page_size, 0, 0, 0));

			/* Illegal flags */
			VOID_RET(int, remap_file_pages((void *)(mapped + page_size), page_size, 0, 0, ~0));

			/* Invalid prot */
			VOID_RET(int, remap_file_pages((void *)(mapped + page_size), page_size, ~0, order[0], 0));
		}
		inc_counter(args);
	} while (keep_stressing(args));

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	rate = (count > 0.0) ? duration / count : 0.0;
	stress_metrics_set(args, 0, "nanosecs per page remap", rate * 1000000000);

	(void)munmap(data, data_size);
	if (mapped)
		(void)munmap((void *)mapped, mapped_size);
	if (unmapped)
		(void)munmap((void *)unmapped, page_size);

	return EXIT_SUCCESS;
}

stressor_info_t stress_remap_info = {
	.stressor = stress_remap,
	.class = CLASS_MEMORY | CLASS_OS,
	.verify = VERIFY_ALWAYS,
	.help = help
};
#else
stressor_info_t stress_remap_info = {
	.stressor = stress_unimplemented,
	.class = CLASS_MEMORY | CLASS_OS,
	.help = help,
	.unimplemented_reason = "built without remap_file_pages() or unsupported for SPARC Linux"
};
#endif
