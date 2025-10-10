/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <systemc>

#include "smmu500.h"

typedef gs::smmu500<> smmu500;
typedef gs::smmu500_tbu<> smmu500_tbu;

void module_register()
{
    GSC_MODULE_REGISTER_C(smmu500);
    GSC_MODULE_REGISTER_C(smmu500_tbu, sc_core::sc_object*);
}