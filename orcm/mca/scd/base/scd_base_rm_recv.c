/*
 * Copyright (c) 2014      Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "orcm_config.h"
#include "orcm/constants.h"
#include "orcm/types.h"

#include "opal/dss/dss.h"
#include "opal/mca/mca.h"
#include "opal/util/output.h"
#include "opal/mca/base/base.h"

#include "orte/types.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"
#include "orte/util/regex.h"

#include "orcm/runtime/orcm_globals.h"
#include "orcm/util/utils.h"
#include "orcm/mca/cfgi/cfgi_types.h"
#include "orcm/mca/scd/base/base.h"

static bool rm_recv_issued=false;

static void orcm_scd_base_rm_recv(int status, orte_process_name_t* sender,
                                  opal_buffer_t* buffer, orte_rml_tag_t tag,
                                  void* cbdata);
static int update_nodestate_byproc(orcm_node_state_t state, opal_list_t *nodelist, hwloc_topology_t topo);
static int update_nodestate_byname(orcm_node_state_t state, char *regexp, hwloc_topology_t topo);

int orcm_scd_base_rm_comm_start(void)
{
    if (rm_recv_issued) {
        return ORCM_SUCCESS;
    }
    
    OPAL_OUTPUT_VERBOSE((5, orcm_scd_base_framework.framework_output,
                         "%s scd:base:rm:receive start comm",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD,
                            ORCM_RML_TAG_RM,
                            ORTE_RML_PERSISTENT,
                            orcm_scd_base_rm_recv,
                            NULL);
    rm_recv_issued = true;
    
    return ORCM_SUCCESS;
}


int orcm_scd_base_rm_comm_stop(void)
{
    if (!rm_recv_issued) {
        return ORCM_SUCCESS;
    }
    
    OPAL_OUTPUT_VERBOSE((5, orcm_scd_base_framework.framework_output,
                         "%s scd:base:rm:receive stop comm",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    orte_rml.recv_cancel(ORTE_NAME_WILDCARD, ORCM_RML_TAG_RM);
    rm_recv_issued = false;
    
    return ORCM_SUCCESS;
}


/* process incoming messages in order of receipt */
static void orcm_scd_base_rm_recv(int status, orte_process_name_t* sender,
                                  opal_buffer_t* buffer, orte_rml_tag_t tag,
                                  void* cbdata)
{
    orcm_rm_cmd_flag_t command;
    int rc, cnt, i, result;
    opal_buffer_t *ans;
    orcm_node_state_t state;
    orte_process_name_t node;
    orcm_alloc_t *alloc;
    orcm_session_t *session = NULL;
    bool found;
    bool have_hwloc_topo;
    hwloc_topology_t topo = NULL;
    hwloc_topology_t t;
    opal_list_t *nodelist;
    orte_namelist_t *nm;
    char *regexp;
    orcm_alloc_tracker_t *trk;

    OPAL_OUTPUT_VERBOSE((5, orcm_scd_base_framework.framework_output,
                         "%s scd:base:rm:receive processing msg",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));

    /* unpack the command */
    cnt = 1;
    if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &command,
                                              &cnt, ORCM_RM_CMD_T))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    if (ORCM_NODESTATE_UPDATE_COMMAND == command) {
        nodelist = OBJ_NEW(opal_list_t);
        OPAL_OUTPUT_VERBOSE((5, orcm_scd_base_framework.framework_output,
                             "%s scd:base:rm:receive got ORCM_NODESTATE_UPDATE_COMMAND",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        cnt = 1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &state,
                                                  &cnt, OPAL_INT8))) {
            ORTE_ERROR_LOG(rc);
            OPAL_LIST_RELEASE(nodelist);
            return;
        }
        if (ORCM_NODE_STATE_UP == state) {
            cnt = 1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &node,
                                                      &cnt, ORTE_NAME))) {
                ORTE_ERROR_LOG(rc);
                OPAL_LIST_RELEASE(nodelist);
                return;
            }

            cnt = 1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &have_hwloc_topo,
                                                      &cnt, OPAL_BOOL))) {
                ORTE_ERROR_LOG(rc);
                OPAL_LIST_RELEASE(nodelist);
                return;
            }
            if (have_hwloc_topo) {
                cnt = 1;
                if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &topo,
                                                          &cnt, OPAL_HWLOC_TOPO))) {
                    ORTE_ERROR_LOG(rc);
                    OPAL_LIST_RELEASE(nodelist);
                    return;
                }
                if(10 < opal_output_get_verbosity(orcm_scd_base_framework.framework_output)) {
                    opal_output(0, "-------------------------------------------");
                    opal_output(0, "%s scd:base:rm:receive RECEIVED NODE %s:",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                ORTE_NAME_PRINT(&node));
                    opal_dss.dump(0, topo, OPAL_HWLOC_TOPO);
                    opal_output(0, "-------------------------------------------");
                }
                found = false;
                for (i = 0; i < orcm_scd_base.topologies.size; i++) {
                    if (NULL == (t = (hwloc_topology_t)opal_pointer_array_get_item(&orcm_scd_base.topologies, i))) {
                        continue;
                    }
                    if (OPAL_EQUAL == opal_dss.compare(topo, t, OPAL_HWLOC_TOPO)) {
                        /* yes - just point to it */
                        OPAL_OUTPUT_VERBOSE((5, orcm_scd_base_framework.framework_output,
                                             "%s TOPOLOGY MATCHES - DISCARDING",
                                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
                        found = true;
                        hwloc_topology_destroy(topo);
                        break;
                    }
                }
                if (!found) {
                    /* nope - add it */
                    OPAL_OUTPUT_VERBOSE((5, orcm_scd_base_framework.framework_output,
                                         "%s NEW TOPOLOGY - ADDING",
                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));

                    opal_pointer_array_add(&orcm_scd_base.topologies, topo);
                    t = topo;
                }
            }

            /* add ourself to the nodelist for state change */
            nm = OBJ_NEW(orte_namelist_t);
            nm->name.jobid = node.jobid;
            nm->name.vpid = node.vpid;
            opal_list_append(nodelist, &nm->super);
            if (ORCM_SUCCESS != (rc = update_nodestate_byproc(state, nodelist, topo))) {
                ORTE_ERROR_LOG(rc);
            }
        } else if (ORCM_NODE_STATE_DOWN == state) {
            cnt = 1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &node,
                                                      &cnt, ORTE_NAME))) {
                ORTE_ERROR_LOG(rc);
                OPAL_LIST_RELEASE(nodelist);
                return;
            }

            /* get all nodes affected by comm failure */
            /* an empty list means we have a leaf node */
            /* this needs to be updated once "healing" is implemented
               beccause this will not be an accurate picture of what
               is really affected by comms failures */
            orcm_util_get_dependents(nodelist, &node);

            /* if list is empty, just add the single node */
            if (opal_list_is_empty(nodelist)) {
                nm = OBJ_NEW(orte_namelist_t);
                nm->name.jobid = node.jobid;
                nm->name.vpid = node.vpid;
                opal_list_append(nodelist, &nm->super);
            }
            if (ORCM_SUCCESS != (rc = update_nodestate_byproc(state, nodelist, NULL))) {
                ORTE_ERROR_LOG(rc);
            }
        } else if (ORCM_NODE_STATE_DRAIN == state) {
            /* someone will expect status results from drain request */
            ans = OBJ_NEW(opal_buffer_t);
            cnt = 1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &regexp,
                                                      &cnt, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                OPAL_LIST_RELEASE(nodelist);
                return;
            }

            OPAL_OUTPUT_VERBOSE((5, orcm_scd_base_framework.framework_output,
                                 "%s scd:base:rm:receive got DRAIN request for %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), regexp));

            if (ORCM_SUCCESS != (rc = update_nodestate_byname(state, regexp, NULL))) {
                ORTE_ERROR_LOG(rc);
            }

            /* send status back to caller */
            result = rc;
            if (OPAL_SUCCESS != (rc = opal_dss.pack(ans, &result,
                                                    1, OPAL_INT))) {
                ORTE_ERROR_LOG(rc);
                OBJ_RELEASE(ans);
                return;
            }

            if (ORTE_SUCCESS !=
                (rc = orte_rml.send_buffer_nb(sender, ans,
                                              ORCM_RML_TAG_RM,
                                              orte_rml_send_callback, NULL))) {
                ORTE_ERROR_LOG(rc);
                OBJ_RELEASE(ans);
                return;
            }
        } else if (ORCM_NODE_STATE_RESUME == state) {
            /* someone will expect status results from resume request */
            ans = OBJ_NEW(opal_buffer_t);
            cnt = 1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &regexp,
                                                      &cnt, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                OPAL_LIST_RELEASE(nodelist);
                return;
            }

            OPAL_OUTPUT_VERBOSE((5, orcm_scd_base_framework.framework_output,
                                 "%s scd:base:rm:receive got RESUME request for %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), regexp));

            if (ORCM_SUCCESS != (rc = update_nodestate_byname(state, regexp, NULL))) {
                ORTE_ERROR_LOG(rc);
            }

            /* send status back to caller */
            result = rc;
            if (OPAL_SUCCESS != (rc = opal_dss.pack(ans, &result,
                                                    1, OPAL_INT))) {
                ORTE_ERROR_LOG(rc);
                OBJ_RELEASE(ans);
                return;
            }

            if (ORTE_SUCCESS !=
                (rc = orte_rml.send_buffer_nb(sender, ans,
                                              ORCM_RML_TAG_RM,
                                              orte_rml_send_callback, NULL))) {
                ORTE_ERROR_LOG(rc);
                OBJ_RELEASE(ans);
                return;
            }
        }

        OPAL_LIST_RELEASE(nodelist);
    } else if (ORCM_STEPD_COMPLETE_COMMAND == command) {
        OPAL_OUTPUT_VERBOSE((5, orcm_scd_base_framework.framework_output,
                             "%s scd:base:rm:receive got ORCM_STEPD_COMPLETE_COMMAND",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        cnt = 1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &alloc,
                                                  &cnt, ORCM_ALLOC))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        
        /* search through the list of running allocations */
        OPAL_LIST_FOREACH(trk, &orcm_scd_base.tracking, orcm_alloc_tracker_t) {
            if (trk->alloc_id == alloc->id) {
                trk->count_checked_in++;
                if (trk->count_checked_in == alloc->min_nodes) {
                    session = OBJ_NEW(orcm_session_t);
                    session->alloc = alloc;
                    session->id = session->alloc->id;
                    ORCM_ACTIVATE_SCD_STATE(session, ORCM_SESSION_STATE_TERMINATED);
                    opal_output(0, "scheduler: all nodes checked in, cancelling session : %ld \n", (long)alloc->id);
                    opal_list_remove_item(&orcm_scd_base.tracking, &trk->super);
                    OBJ_RELEASE(trk);
                }
                return;
            }
        }
        opal_output(0, "scheduler: couldn't find running allocation to cancel : %ld!\n", (long)alloc->id);
    } else if (ORCM_SET_POWER_COMMAND == command) {
        OPAL_OUTPUT_VERBOSE((5, orcm_scd_base_framework.framework_output,
                             "%s scd:base:rm:receive got ORCM_SET_POWER_COMMAND",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        cnt = 1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &alloc,
                                                  &cnt, ORCM_ALLOC))) {
            ORTE_ERROR_LOG(rc);
            return;
        }

        ans = OBJ_NEW(opal_buffer_t);

        if (OPAL_SUCCESS != (rc = opal_dss.pack(ans, &command,
                                            1, ORCM_RM_CMD_T))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(ans);
            return;
        }
        if (OPAL_SUCCESS != (rc = opal_dss.pack(ans, &alloc,
                                                1, ORCM_ALLOC))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(ans);
            return;
        }

        if (ORTE_SUCCESS !=
            (rc = orte_rml.send_buffer_nb(&alloc->hnp, ans,
                                          ORCM_RML_TAG_PWRMGMT_BASE,
                                          orte_rml_send_callback, NULL))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(ans);
            return;
        }
    }

    return;
}

static int update_nodestate_byproc(orcm_node_state_t state, opal_list_t *nodelist, hwloc_topology_t topo)
{
    int i;
    orcm_node_t *nodeptr;
    orte_namelist_t *n;
    bool found;

    /* set each node to state */
    found = false;
    OPAL_LIST_FOREACH(n, nodelist, orte_namelist_t) {
        for (i = 0; i < orcm_scd_base.nodes.size; i++) {
            if (NULL == (nodeptr =
                         (orcm_node_t*)opal_pointer_array_get_item(&orcm_scd_base.nodes,
                                                                   i))) {
                             continue;
                         }
            if (OPAL_EQUAL == orte_util_compare_name_fields(ORTE_NS_CMP_ALL,
                                                            &nodeptr->daemon,
                                                            &n->name)) {
                OPAL_OUTPUT_VERBOSE((1, orcm_scd_base_framework.framework_output,
                                     "%s scd:base:rm:update_nodestate_byproc Setting node %s to state %i (%s)",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_NAME_PRINT(&n->name),
                                     (int)state,
                                     orcm_node_state_to_str(state)));
                found = true;
                nodeptr->state = state;
                
                /* associate node topology with node */
                if (ORCM_NODE_STATE_UP == state) {
                    nodeptr->topology = topo;
                }
                
                /* if the node is coming online, reset the scheduling state
                 only if its either undefined or unknown */
                if ((ORCM_NODE_STATE_UP == state) &&
                    ((ORCM_SCD_NODE_STATE_UNDEF == nodeptr->scd_state) ||
                     (ORCM_SCD_NODE_STATE_UNKNOWN == nodeptr->scd_state))) {
                        nodeptr->scd_state = ORCM_SCD_NODE_STATE_UNALLOC;
                    }
                break;
            }
        }
    }
    
    if (!found) {
        OPAL_OUTPUT_VERBOSE((1, orcm_scd_base_framework.framework_output,
                             "%s scd:base:rm:update_nodestate_byproc Couldn't find node(s) to update state %i (%s)",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (int)state,
                             orcm_node_state_to_str(state)));
        
        return ORCM_ERR_NOT_FOUND;
    }
    return ORCM_SUCCESS;
}

static int update_nodestate_byname(orcm_node_state_t state, char *regexp, hwloc_topology_t topo)
{
    int cnt, i, j, rc;
    orcm_node_t *nodeptr;
    char **nodenames = NULL;
    bool found = false;
    orcm_node_state_t newstate;
    
    if (ORCM_NODE_STATE_RESUME == state) {
        newstate = ORCM_NODE_STATE_UP;
    } else {
        newstate = state;
    }
    
    if (ORTE_SUCCESS !=
        (rc = orte_regex_extract_node_names(regexp, &nodenames))) {
        ORTE_ERROR_LOG(rc);
        opal_argv_free(nodenames);
        return rc;
    }
    cnt = opal_argv_count(nodenames);
    for (i = 0; i < cnt; i++) {
        for (j = 0; j < orcm_scd_base.nodes.size; j++) {
            if (NULL == (nodeptr =
                         (orcm_node_t*)opal_pointer_array_get_item(&orcm_scd_base.nodes,
                                                                   j))) {
                             continue;
                         }
            if (0 == strcmp(nodeptr->name, nodenames[i])) {
                OPAL_OUTPUT_VERBOSE((1, orcm_scd_base_framework.framework_output,
                                     "%s scd:base:rm:update_nodestate_byname Setting node %s to state %i (%s)",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_NAME_PRINT(&nodeptr->daemon),
                                     (int)state,
                                     orcm_node_state_to_str(state)));
                found = true;
                nodeptr->state = newstate;
                
                /* associate node topology with node */
                if (ORCM_NODE_STATE_UP == state) {
                    nodeptr->topology = topo;
                }
                
                /* if the node is coming online, reset the scheduling state
                 only if its either undefined or unknown */
                if ((ORCM_NODE_STATE_UP == state) &&
                    ((ORCM_SCD_NODE_STATE_UNDEF == nodeptr->scd_state) ||
                     (ORCM_SCD_NODE_STATE_UNKNOWN == nodeptr->scd_state))) {
                        nodeptr->scd_state = ORCM_SCD_NODE_STATE_UNALLOC;
                    }
                break;
            }
        }
    }
    opal_argv_free(nodenames);
    if (!found) {
        OPAL_OUTPUT_VERBOSE((1, orcm_scd_base_framework.framework_output,
                             "%s scd:base:rm:update_nodestate_byname Couldn't find node(s) to update state %i (%s)",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (int)state,
                             orcm_node_state_to_str(state)));

        return ORCM_ERR_NOT_FOUND;
    }
    return ORCM_SUCCESS;
}
