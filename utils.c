/*
 * utils.c
 *
 *  Created on: Jul 17, 2019
 *      Author: zhangzm
 */
#include "common.h"
#include "stdint.h"

int atomic_inc(int* nb)
{
	return __sync_fetch_and_add(nb, 1);
}

int atomic_dec(int* nb)
{
	return __sync_fetch_and_add(nb, -1);
}

int atomic_cas(void** v, void* pre_v, void* new_v)
{
	return __sync_bool_compare_and_swap(v, pre_v, new_v);
}


