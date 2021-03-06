/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved. 
 * Copyright (c) 2012      Los Alamos National Security, Inc. All rights reserved.
 *
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */
/**
 * @file
 *
 * Heartbeat sensor 
 */
#ifndef ORCM_SENSOR_HEARTBEAT_H
#define ORCM_SENSOR_HEARTBEAT_H

#include "orcm_config.h"

#include "orcm/mca/sensor/sensor.h"

BEGIN_C_DECLS

ORCM_MODULE_DECLSPEC extern orcm_sensor_base_component_t mca_sensor_heartbeat_component;
extern orcm_sensor_base_module_t orcm_sensor_heartbeat_module;


END_C_DECLS

#endif
