/*
 * Copyright (c) 2012-2013 Los Alamos National Security, Inc. All rights reserved.
 * Copyright (c) 2013-2014 Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 *
 */

#include "orcm_config.h"
#include "orcm/constants.h"

#include <string.h>
#include <sys/types.h>
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#include <stdio.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <sys/time.h>
#include <time.h>

#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>

#include "opal_stdint.h"
#include "opal/util/argv.h"
#include "opal/util/error.h"

#include "orcm/runtime/orcm_globals.h"

#include "orcm/mca/db/base/base.h"

#include "db_odbc.h"

/* Module API functions */
static int odbc_init(struct orcm_db_base_module_t *imod);
static void odbc_finalize(struct orcm_db_base_module_t *imod);
static int odbc_store_sample(struct orcm_db_base_module_t *imod,
                             const char *primary_key,
                             opal_list_t *kvs);
static int odbc_record_data_samples(struct orcm_db_base_module_t *imod,
                                    const char *hostname,
                                    const struct timeval *time_stamp,
                                    const char *data_group,
                                    opal_list_t *samples);
static int odbc_update_node_features(struct orcm_db_base_module_t *imod,
                                     const char *hostname,
                                     opal_list_t *features);
static int odbc_record_diag_test(struct orcm_db_base_module_t *imod,
                                 const char *hostname,
                                 const char *diag_type,
                                 const char *diag_subtype,
                                 const struct tm *start_time,
                                 const struct tm *end_time,
                                 const int *component_index,
                                 const char *test_result,
                                 opal_list_t *test_params);
static int odbc_fetch(struct orcm_db_base_module_t *imod,
                      const char *primary_key,
                      const char *key,
                      opal_list_t *kvs);
static int odbc_remove(struct orcm_db_base_module_t *imod,
                      const char *primary_key,
                      const char *key);

/* Internal helper functions */
static void odbc_error_info(SQLSMALLINT handle_type, SQLHANDLE handle);
static void tm_to_sql_timestamp(SQL_TIMESTAMP_STRUCT *sql_timestamp,
                                const struct tm *time_info);
static void tv_to_sql_timestamp(SQL_TIMESTAMP_STRUCT *sql_timestamp,
                                const struct timeval *time);

mca_db_odbc_module_t mca_db_odbc_module = {
    {
        odbc_init,
        odbc_finalize,
        odbc_store_sample,
        odbc_record_data_samples,
        odbc_update_node_features,
        odbc_record_diag_test,
        NULL,
        odbc_fetch,
        odbc_remove
    },
};

#define ERROR_MSG_FMT_INIT(mod, msg, ...) \
    opal_output(0, "***********************************************"); \
    opal_output(0, "db:odbc: Connection failed: "); \
    opal_output(0, msg, ##__VA_ARGS__); \
    opal_output(0, "\tDSN: %s", mod->odbcdsn); \
    opal_output(0, "***********************************************");

static int odbc_init(struct orcm_db_base_module_t *imod)
{
    mca_db_odbc_module_t *mod = (mca_db_odbc_module_t*)imod;
    char **login = NULL;

    SQLRETURN ret;

    /* break the user info into its login parts */
    login = opal_argv_split(mod->user, ':');
    if (2 != opal_argv_count(login)) {
        ERROR_MSG_FMT_INIT(mod, "User info is invalid: %s", mod->user);
        opal_argv_free(login);
        return ORCM_ERR_BAD_PARAM;
    }

    ret = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &mod->envhandle);
    if (!(SQL_SUCCEEDED(ret))) {
        opal_argv_free(login);
        mod->envhandle = NULL;
        ERROR_MSG_FMT_INIT(mod, "SQLAllocHandle returned: %d", ret);
        return ORCM_ERR_CONNECTION_FAILED;
    }

    ret = SQLSetEnvAttr(mod->envhandle, SQL_ATTR_ODBC_VERSION,
                        (SQLPOINTER)SQL_OV_ODBC3, 0);
    if (!(SQL_SUCCEEDED(ret))) {
        opal_argv_free(login);
        SQLFreeHandle(SQL_HANDLE_ENV, mod->envhandle);
        mod->envhandle = NULL;
        ERROR_MSG_FMT_INIT(mod, "SQLSetEnvAttr returned: %d", ret);
        return ORCM_ERR_CONNECTION_FAILED;
    }

    ret = SQLAllocHandle(SQL_HANDLE_DBC, mod->envhandle, &mod->dbhandle);
    if (!(SQL_SUCCEEDED(ret))) {
        opal_argv_free(login);
        mod->dbhandle = NULL;
        SQLFreeHandle(SQL_HANDLE_ENV, mod->envhandle);
        mod->envhandle = NULL;
        ERROR_MSG_FMT_INIT(mod, "SQLAllocHandle returned: %d", ret);
        return ORCM_ERR_CONNECTION_FAILED;
    }

    ret = SQLSetConnectAttr(mod->dbhandle, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);
    if (!(SQL_SUCCEEDED(ret))) {
        opal_argv_free(login);
        SQLFreeHandle(SQL_HANDLE_DBC, mod->dbhandle);
        mod->dbhandle = NULL;
        SQLFreeHandle(SQL_HANDLE_ENV, mod->envhandle);
        mod->envhandle = NULL;
        ERROR_MSG_FMT_INIT(mod, "SQLSetConnectAttr returned: %d", ret);
        return ORCM_ERR_CONNECTION_FAILED;
    }

    ret = SQLConnect(mod->dbhandle, (SQLCHAR *)mod->odbcdsn, SQL_NTS,
                     (SQLCHAR *)login[0], SQL_NTS,
                     (SQLCHAR *)login[1], SQL_NTS);
    if (!(SQL_SUCCEEDED(ret))) {
        opal_argv_free(login);
        SQLFreeHandle(SQL_HANDLE_DBC, mod->dbhandle);
        mod->dbhandle = NULL;
        SQLFreeHandle(SQL_HANDLE_ENV, mod->envhandle);
        mod->envhandle = NULL;
        ERROR_MSG_FMT_INIT(mod, "SQLConnect returned: %d", ret);
        return ORCM_ERR_CONNECTION_FAILED;
    }

    opal_argv_free(login);

    opal_output_verbose(5, orcm_db_base_framework.framework_output,
                        "db:odbc: Connection established to %s",
                        mod->odbcdsn);

    return ORCM_SUCCESS;
}

static void odbc_finalize(struct orcm_db_base_module_t *imod)
{
    mca_db_odbc_module_t *mod = (mca_db_odbc_module_t*)imod;

    if (NULL != mod->table) {
        free(mod->table);
    }
    if (NULL != mod->user) {
        free(mod->user);
    }

    if (NULL != mod->dbhandle) {
        SQLFreeHandle(SQL_HANDLE_DBC, mod->dbhandle);
    }

    if (NULL != mod->envhandle) {
        SQLFreeHandle(SQL_HANDLE_ENV, mod->envhandle);
    }
}

#define ERR_MSG_STORE(msg) \
    opal_output(0, "***********************************************"); \
    opal_output(0, "db:odbc: Unable to record data sample: "); \
    opal_output(0, msg); \
    opal_output(0, "***********************************************");

#define ERR_MSG_FMT_STORE(msg, ...) \
    opal_output(0, "***********************************************"); \
    opal_output(0, "db:odbc: Unable to record data sample: "); \
    opal_output(0, msg, ##__VA_ARGS__); \
    opal_output(0, "***********************************************");

#define ERR_MSG_FMT_SQL_STORE(handle_type, handle, msg, ...) \
    opal_output(0, "***********************************************"); \
    opal_output(0, "db:odbc: Unable to record data sample: "); \
    opal_output(0, msg, ##__VA_ARGS__); \
    opal_output(0, "ODBC error details:"); \
    odbc_error_info(handle_type, handle); \
    opal_output(0, "***********************************************");

static int odbc_store_sample(struct orcm_db_base_module_t *imod,
                             const char *data_group,
                             opal_list_t *kvs)
{
    mca_db_odbc_module_t *mod = (mca_db_odbc_module_t*)imod;
    opal_value_t *kv;
    opal_value_t *timestamp_item = NULL;
    opal_value_t *hostname_item = NULL;
    char *sampletime_str;
    struct tm time_info;

    SQL_TIMESTAMP_STRUCT sampletime;
    char hostname[256];
    char **data_item_argv;
    int argv_count;
    orcm_db_item_t item;
    orcm_db_item_type_t prev_type = ORCM_DB_ITEM_INTEGER;
    int change_value_binding = 1;
    SQLLEN null_len = SQL_NULL_DATA;

    SQLRETURN ret;
    SQLHSTMT stmt;

    if (NULL == data_group) {
        ERR_MSG_STORE("No data group specified");
        return ORCM_ERR_BAD_PARAM;
    }

    if (NULL == kvs) {
        ERR_MSG_STORE("No value list specified");
        return ORCM_ERR_BAD_PARAM;
    }

    /* First, retrieve the time stamp and the hostname from the list */
    OPAL_LIST_FOREACH(kv, kvs, opal_value_t) {
        if (!strcmp(kv->key, "ctime")) {
            switch (kv->type) {
            case OPAL_TIMEVAL:
            case OPAL_TIME:
                tv_to_sql_timestamp(&sampletime, &kv->data.tv);
                break;
            case OPAL_STRING:
                sampletime_str = kv->data.string;
                /* Note: assuming "%F %T%z" format and ignoring sub second
                resolution when passed as a string */
                strptime(sampletime_str, "%F %T%z", &time_info);
                tm_to_sql_timestamp(&sampletime, &time_info);
                break;
            default:
                ERR_MSG_STORE("Invalid value type specified for time stamp");
                return ORCM_ERR_BAD_PARAM;
            }
            timestamp_item = kv;
        } else if (!strcmp(kv->key, "hostname")) {
            if (OPAL_STRING == kv->type) {
                strncpy(hostname, kv->data.string, sizeof(hostname) - 1);
                hostname[sizeof(hostname) - 1] = '\0';
            } else {
                ERR_MSG_STORE("Invalid value type specified for hostname");
                return ORCM_ERR_BAD_PARAM;
            }
            hostname_item = kv;
        }

        if (NULL != timestamp_item && NULL != hostname_item) {
            break;
        }
    }

    if (NULL == timestamp_item) {
        ERR_MSG_STORE("No time stamp provided");
        return ORCM_ERR_BAD_PARAM;
    }
    if (NULL == hostname_item) {
        ERR_MSG_STORE("No hostname provided");
        return ORCM_ERR_BAD_PARAM;
    }

    /* Remove these from the list to avoid processing them again */
    opal_list_remove_item(kvs, (opal_list_item_t *)timestamp_item);
    opal_list_remove_item(kvs, (opal_list_item_t *)hostname_item);
    OBJ_RELEASE(timestamp_item);
    OBJ_RELEASE(hostname_item);

    ret = SQLAllocHandle(SQL_HANDLE_STMT, mod->dbhandle, &stmt);
    if (!(SQL_SUCCEEDED(ret))) {
        ERR_MSG_FMT_STORE("SQLAllocHandle returned: %d", ret);
        return ORCM_ERROR;
    }

    /*
     * 1.- hostname
     * 2.- data group
     * 3.- data item
     * 4.- time stamp
     * 5.- data type ID
     * 6.- integer value
     * 7.- real value
     * 8.- string value
     * 9.- units
     */
    ret = SQLPrepare(stmt,
                     (SQLCHAR *)
                     "{call record_data_sample(?, ?, ?, ?, ?, ?, ?, ?, ?)}",
                     SQL_NTS);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLPrepare returned: %d", ret);
        return ORCM_ERROR;
    }

    /* Bind hostname parameter. */
    ret = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, (SQLPOINTER)hostname, strlen(hostname), NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLBindParameter 1 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind data group parameter. */
    ret = SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, (SQLPOINTER)data_group, strlen(data_group),
                           NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLBindParameter 2 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind time stamp parameter. */
    ret = SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_TYPE_TIMESTAMP,
                           SQL_TYPE_TIMESTAMP, 0, 0, (SQLPOINTER)&sampletime,
                           sizeof(sampletime), NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLBindParameter 4 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind data type parameter. */
    ret = SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                           0, 0, (SQLPOINTER)&item.opal_type,
                           sizeof(item.opal_type), NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLBindParameter 5 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind integer value parameter (assuming the value is integer for now). */
    ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT,
                           0, 0, (SQLPOINTER)&item.value.value_int,
                           sizeof(item.value.value_int), NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLBindParameter 6 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind real value parameter (assuming NULL for now). */
    ret = SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE,
                           0, 0, NULL, 0, &null_len);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLBindParameter 7 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind string value parameter (assuming NULL for now). */
    ret = SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, NULL, 0, &null_len);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLBindParameter 8 returned: %d", ret);
        return ORCM_ERROR;
    }

    OPAL_LIST_FOREACH(kv, kvs, opal_value_t) {
        ret = opal_value_to_orcm_db_item(kv, &item);

        if (ORCM_SUCCESS != ret) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            ERR_MSG_STORE("Unsupported value type");
            return ORCM_ERR_NOT_SUPPORTED;
        }
        change_value_binding = prev_type != item.item_type;
        prev_type = item.item_type;

        /* kv->key will contain: <data item>:<units> */
        data_item_argv = opal_argv_split(kv->key, ':');
        argv_count = opal_argv_count(data_item_argv);
        if (argv_count == 0) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            opal_argv_free(data_item_argv);
            ERR_MSG_STORE("No data item specified");
            return ORCM_ERR_BAD_PARAM;
        }
        /* Bind the data item parameter. */
        ret = SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR,
                               SQL_VARCHAR, 0, 0, (SQLPOINTER)data_item_argv[0],
                               strlen(data_item_argv[0]), NULL);
        if (!(SQL_SUCCEEDED(ret))) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            opal_argv_free(data_item_argv);
            ERR_MSG_FMT_STORE("SQLBindParameter 3 returned: %d", ret);
            return ORCM_ERROR;
        }

        if (argv_count > 1) {
            /* Bind the units parameter. */
            ret = SQLBindParameter(stmt, 9, SQL_PARAM_INPUT, SQL_C_CHAR,
                                   SQL_VARCHAR, 0, 0,
                                   (SQLPOINTER)data_item_argv[1],
                                   strlen(data_item_argv[1]), NULL);
        } else {
            /* No units provided, bind NULL. */
            ret = SQLBindParameter(stmt, 9, SQL_PARAM_INPUT, SQL_C_CHAR,
                                   SQL_VARCHAR, 0, 0, NULL, 0, &null_len);
        }
        if (!(SQL_SUCCEEDED(ret))) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            opal_argv_free(data_item_argv);
            ERR_MSG_FMT_STORE("SQLBindParameter 9 returned: %d", ret);
            return ORCM_ERROR;
        }

        if (change_value_binding) {
            switch (item.item_type) {
            case ORCM_DB_ITEM_INTEGER:
                /* Value is integer, bind to the appropriate value. */
                ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                                       SQL_BIGINT, 0, 0,
                                       (SQLPOINTER)&item.value.value_int,
                                       sizeof(item.value.value_int), NULL);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    opal_argv_free(data_item_argv);
                    ERR_MSG_FMT_STORE("SQLBindParameter 6 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the real value. */
                ret = SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_DOUBLE,
                                       SQL_DOUBLE, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    opal_argv_free(data_item_argv);
                    ERR_MSG_FMT_STORE("SQLBindParameter 7 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the string value. */
                ret = SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR,
                                       SQL_VARCHAR, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    opal_argv_free(data_item_argv);
                    ERR_MSG_FMT_STORE("SQLBindParameter 8 returned: %d", ret);
                    return ORCM_ERROR;
                }
                break;
            case ORCM_DB_ITEM_REAL:
                /* Pass NULL for the integer value. */
                ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                                       SQL_BIGINT, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    opal_argv_free(data_item_argv);
                    ERR_MSG_FMT_STORE("SQLBindParameter 6 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Value is real, bind to the appropriate value. */
                ret = SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_DOUBLE,
                                       SQL_DOUBLE, 0, 0,
                                       (SQLPOINTER)&item.value.value_real,
                                       sizeof(item.value.value_real), NULL);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    opal_argv_free(data_item_argv);
                    ERR_MSG_FMT_STORE("SQLBindParameter 7 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the string value. */
                ret = SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR,
                                       SQL_VARCHAR, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    opal_argv_free(data_item_argv);
                    ERR_MSG_FMT_STORE("SQLBindParameter 8 returned: %d", ret);
                    return ORCM_ERROR;
                }
                break;
            case ORCM_DB_ITEM_STRING:
                /* Pass NULL for the integer value. */
                ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                                       SQL_BIGINT, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    opal_argv_free(data_item_argv);
                    ERR_MSG_FMT_STORE("SQLBindParameter 6 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the real value. */
                ret = SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_DOUBLE,
                                       SQL_DOUBLE, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    opal_argv_free(data_item_argv);
                    ERR_MSG_FMT_STORE("SQLBindParameter 7 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Value is string, bind to the appropriate value. */
                ret = SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR,
                                       SQL_VARCHAR, 0, 0,
                                       (SQLPOINTER)item.value.value_str,
                                       strlen(item.value.value_str), NULL);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    opal_argv_free(data_item_argv);
                    ERR_MSG_FMT_STORE("SQLBindParameter 8 returned: %d", ret);
                    return ORCM_ERROR;
                }
                break;
            default:
                SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                opal_argv_free(data_item_argv);
                ERR_MSG_STORE("An unexpected error has occurred while "
                              "processing the values");
                return ORCM_ERROR;
            }
        } else if (ORCM_DB_ITEM_STRING == item.item_type) {
            /* No need to change the binding for all the values, just update
             * the string binding. */
            ret = SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR,
                                   SQL_VARCHAR, 0, 0,
                                   (SQLPOINTER)item.value.value_str,
                                   strlen(item.value.value_str), NULL);
            if (!(SQL_SUCCEEDED(ret))) {
                SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                ERR_MSG_FMT_STORE("SQLBindParameter 8 returned: %d", ret);
                return ORCM_ERROR;
            }
        }

        ret = SQLExecute(stmt);
        if (!(SQL_SUCCEEDED(ret))) {
            ERR_MSG_FMT_SQL_STORE(SQL_HANDLE_STMT, stmt,
                                  "SQLExecute returned: %d", ret);
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            opal_argv_free(data_item_argv);
            return ORCM_ERROR;
        }
        
        SQLCloseCursor(stmt);

        opal_argv_free(data_item_argv);
    }

    opal_output_verbose(2, orcm_db_base_framework.framework_output,
                        "odbc_store_sample succeeded");

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    return ORCM_SUCCESS;
}

static int odbc_record_data_samples(struct orcm_db_base_module_t *imod,
                                    const char *hostname,
                                    const struct timeval *time_stamp,
                                    const char *data_group,
                                    opal_list_t *samples)
{
    mca_db_odbc_module_t *mod = (mca_db_odbc_module_t*)imod;
    orcm_metric_value_t *mv;

    SQL_TIMESTAMP_STRUCT sampletime;
    orcm_db_item_t item;
    orcm_db_item_type_t prev_type = ORCM_DB_ITEM_INTEGER;
    int change_value_binding = 1;
    SQLLEN null_len = SQL_NULL_DATA;

    SQLRETURN ret;
    SQLHSTMT stmt;

    if (NULL == data_group) {
        ERR_MSG_STORE("No data group provided");
        return ORCM_ERR_BAD_PARAM;
    }

    if (NULL == hostname) {
        ERR_MSG_STORE("No hostname provided");
        return ORCM_ERR_BAD_PARAM;
    }

    if (NULL == time_stamp) {
        ERR_MSG_STORE("No time stamp provided");
        return ORCM_ERR_BAD_PARAM;
    }

    if (NULL == samples) {
        ERR_MSG_STORE("No value list provided");
        return ORCM_ERR_BAD_PARAM;
    }

    tv_to_sql_timestamp(&sampletime, time_stamp);

    ret = SQLAllocHandle(SQL_HANDLE_STMT, mod->dbhandle, &stmt);
    if (!(SQL_SUCCEEDED(ret))) {
        ERR_MSG_FMT_STORE("SQLAllocHandle returned: %d", ret);
        return ORCM_ERROR;
    }

    /*
     * 1.- hostname
     * 2.- data group
     * 3.- data item
     * 4.- time stamp
     * 5.- data type ID
     * 6.- integer value
     * 7.- real value
     * 8.- string value
     * 9.- units
     */
    ret = SQLPrepare(stmt,
                     (SQLCHAR *)
                     "{call record_data_sample(?, ?, ?, ?, ?, ?, ?, ?, ?)}",
                     SQL_NTS);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLPrepare returned: %d", ret);
        return ORCM_ERROR;
    }

    /* Bind hostname parameter. */
    ret = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, (SQLPOINTER)hostname, strlen(hostname), NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLBindParameter 1 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind data group parameter. */
    ret = SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, (SQLPOINTER)data_group, strlen(data_group),
                           NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLBindParameter 2 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind time stamp parameter. */
    ret = SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_TYPE_TIMESTAMP,
                           SQL_TYPE_TIMESTAMP, 0, 0, (SQLPOINTER)&sampletime,
                           sizeof(sampletime), NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLBindParameter 4 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind data type parameter. */
    ret = SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                           0, 0, (SQLPOINTER)&item.opal_type,
                           sizeof(item.opal_type), NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLBindParameter 5 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind integer value parameter (assuming the value is integer for now). */
    ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT,
                           0, 0, (SQLPOINTER)&item.value.value_int,
                           sizeof(item.value.value_int), NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLBindParameter 6 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind real value parameter (assuming NULL for now). */
    ret = SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE,
                           0, 0, NULL, 0, &null_len);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLBindParameter 7 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind string value parameter (assuming NULL for now). */
    ret = SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, NULL, 0, &null_len);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_STORE("SQLBindParameter 8 returned: %d", ret);
        return ORCM_ERROR;
    }

    OPAL_LIST_FOREACH(mv, samples, orcm_metric_value_t) {
        if (NULL == mv->value.key || 0 == strlen(mv->value.key)) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            ERR_MSG_STORE("Key or data item name not provided for value");
            return ORCM_ERR_BAD_PARAM;
        }

        ret = opal_value_to_orcm_db_item(&mv->value, &item);
        if (ORCM_SUCCESS != ret) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            ERR_MSG_STORE("Unsupported value type");
            return ORCM_ERR_NOT_SUPPORTED;
        }
        change_value_binding = prev_type != item.item_type;
        prev_type = item.item_type;

        /* Bind the data item parameter. */
        ret = SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR,
                               SQL_VARCHAR, 0, 0, (SQLPOINTER)mv->value.key,
                               strlen(mv->value.key), NULL);
        if (!(SQL_SUCCEEDED(ret))) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            ERR_MSG_FMT_STORE("SQLBindParameter 3 returned: %d", ret);
            return ORCM_ERROR;
        }

        if (NULL != mv->units) {
            /* Bind the units parameter. */
            ret = SQLBindParameter(stmt, 9, SQL_PARAM_INPUT, SQL_C_CHAR,
                                   SQL_VARCHAR, 0, 0,
                                   (SQLPOINTER)mv->units,
                                   strlen(mv->units), NULL);
        } else {
            /* No units provided, bind NULL. */
            ret = SQLBindParameter(stmt, 9, SQL_PARAM_INPUT, SQL_C_CHAR,
                                   SQL_VARCHAR, 0, 0, NULL, 0, &null_len);
        }
        if (!(SQL_SUCCEEDED(ret))) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            ERR_MSG_FMT_STORE("SQLBindParameter 9 returned: %d", ret);
            return ORCM_ERROR;
        }

        if (change_value_binding) {
            switch (item.item_type) {
            case ORCM_DB_ITEM_INTEGER:
                /* Value is integer, bind to the appropriate value. */
                ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                                       SQL_BIGINT, 0, 0,
                                       (SQLPOINTER)&item.value.value_int,
                                       sizeof(item.value.value_int), NULL);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_STORE("SQLBindParameter 6 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the real value. */
                ret = SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_DOUBLE,
                                       SQL_DOUBLE, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_STORE("SQLBindParameter 7 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the string value. */
                ret = SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR,
                                       SQL_VARCHAR, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_STORE("SQLBindParameter 8 returned: %d", ret);
                    return ORCM_ERROR;
                }
                break;
            case ORCM_DB_ITEM_REAL:
                /* Pass NULL for the integer value. */
                ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                                       SQL_BIGINT, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_STORE("SQLBindParameter 6 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Value is real, bind to the appropriate value. */
                ret = SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_DOUBLE,
                                       SQL_DOUBLE, 0, 0,
                                       (SQLPOINTER)&item.value.value_real,
                                       sizeof(item.value.value_real), NULL);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_STORE("SQLBindParameter 7 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the string value. */
                ret = SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR,
                                       SQL_VARCHAR, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_STORE("SQLBindParameter 8 returned: %d", ret);
                    return ORCM_ERROR;
                }
                break;
            case ORCM_DB_ITEM_STRING:
                /* Pass NULL for the integer value. */
                ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                                       SQL_BIGINT, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_STORE("SQLBindParameter 6 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the real value. */
                ret = SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_DOUBLE,
                                       SQL_DOUBLE, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_STORE("SQLBindParameter 7 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Value is string, bind to the appropriate value. */
                ret = SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR,
                                       SQL_VARCHAR, 0, 0,
                                       (SQLPOINTER)item.value.value_str,
                                       strlen(item.value.value_str), NULL);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_STORE("SQLBindParameter 8 returned: %d", ret);
                    return ORCM_ERROR;
                }
                break;
            default:
                SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                ERR_MSG_STORE("An unexpected error has occurred while "
                              "processing the values");
                return ORCM_ERROR;
            }
        } else if (ORCM_DB_ITEM_STRING == item.item_type) {
            /* No need to change the binding for all the values, just update
             * the string binding. */
            ret = SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR,
                                   SQL_VARCHAR, 0, 0,
                                   (SQLPOINTER)item.value.value_str,
                                   strlen(item.value.value_str), NULL);
            if (!(SQL_SUCCEEDED(ret))) {
                SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                ERR_MSG_FMT_STORE("SQLBindParameter 8 returned: %d", ret);
                return ORCM_ERROR;
            }
        }

        ret = SQLExecute(stmt);
        if (!(SQL_SUCCEEDED(ret))) {
            ERR_MSG_FMT_SQL_STORE(SQL_HANDLE_STMT, stmt,
                                  "SQLExecute returned: %d", ret);
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            return ORCM_ERROR;
        }

        SQLCloseCursor(stmt);
    }

    opal_output_verbose(2, orcm_db_base_framework.framework_output,
                        "odbc_record_data_samples succeeded");

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    return ORCM_SUCCESS;
}

#define ERR_MSG_UNF(msg) \
    opal_output(0, "***********************************************"); \
    opal_output(0, "db:odbc: Unable to update node features"); \
    opal_output(0, msg); \
    opal_output(0, "***********************************************");

#define ERR_MSG_FMT_UNF(msg, ...) \
    opal_output(0, "***********************************************"); \
    opal_output(0, "db:odbc: Unable to update node features"); \
    opal_output(0, msg, ##__VA_ARGS__); \
    opal_output(0, "***********************************************");

#define ERR_MSG_FMT_SQL_UNF(handle_type, handle, msg, ...) \
    opal_output(0, "***********************************************"); \
    opal_output(0, "db:odbc: Unable to update node features"); \
    opal_output(0, msg, ##__VA_ARGS__); \
    opal_output(0, "ODBC error details:"); \
    odbc_error_info(handle_type, handle); \
    opal_output(0, "***********************************************");

static int odbc_update_node_features(struct orcm_db_base_module_t *imod,
                                     const char *hostname,
                                     opal_list_t *features)
{
    mca_db_odbc_module_t *mod = (mca_db_odbc_module_t*)imod;
    orcm_metric_value_t *mv;

    orcm_db_item_t item;
    orcm_db_item_type_t prev_type = ORCM_DB_ITEM_INTEGER;
    int change_value_binding = 1;
    SQLLEN null_len = SQL_NULL_DATA;

    SQLRETURN ret;
    SQLHSTMT stmt;

    if (NULL == hostname) {
        ERR_MSG_UNF("No hostname provided");
        return ORCM_ERR_BAD_PARAM;
    }

    if (NULL == features) {
        ERR_MSG_UNF("No node features provided");
        return ORCM_ERR_BAD_PARAM;
    }

    ret = SQLAllocHandle(SQL_HANDLE_STMT, mod->dbhandle, &stmt);
    if (!(SQL_SUCCEEDED(ret))) {
        ERR_MSG_FMT_UNF("SQLAllocHandle returned: %d", ret);
        return ORCM_ERROR;
    }

    /*
     * 1 p_hostname character varying,
     * 2 p_feature character varying,
     * 3 p_data_type_id integer,
     * 4 p_value_int bigint,
     * 5 p_value_real double precision,
     * 6 p_value_str character varying,
     * 7 p_units character varying
     * */
    ret = SQLPrepare(stmt,
                     (SQLCHAR *)"{call set_node_feature(?, ?, ?, ?, ?, ?, ?)}",
                     SQL_NTS);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_UNF("SQLPrepare returned: %d", ret);
        return ORCM_ERROR;
    }

    /* Bind hostname parameter. */
    ret = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, (SQLPOINTER)hostname, strlen(hostname), NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_UNF("SQLBindParameter 1 returned: %d", ret);
        return ORCM_ERROR;
    }

    /* Bind data type parameter. */
    ret = SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                           0, 0, (SQLPOINTER)&item.opal_type,
                           sizeof(item.opal_type), NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_UNF("SQLBindParameter 3 returned: %d", ret);
        return ORCM_ERROR;
    }

    /* Bind integer value parameter (assuming the value is integer for now). */
    ret = SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                           SQL_BIGINT, 0, 0, (SQLPOINTER)&item.value.value_int,
                           sizeof(item.value.value_int), NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_UNF("SQLBindParameter 4 returned: %d", ret);
        return ORCM_ERROR;
    }

    /* Bind real value parameter (assuming NULL for now). */
    ret = SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE,
                           0, 0, NULL, 0, &null_len);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_UNF("SQLBindParameter 5 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind string value parameter (assuming NULL for now). */
    ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, NULL, 0, &null_len);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_UNF("SQLBindParameter 6 returned: %d", ret);
        return ORCM_ERROR;
    }

    OPAL_LIST_FOREACH(mv, features, orcm_metric_value_t) {
        if (NULL == mv->value.key || 0 == strlen(mv->value.key)) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            ERR_MSG_UNF("Key or node feature name not provided for value");
            return ORCM_ERR_BAD_PARAM;
        }

        ret = opal_value_to_orcm_db_item(&mv->value, &item);
        if (ORCM_SUCCESS != ret) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            ERR_MSG_UNF("Unsupported value type");
            return ORCM_ERR_NOT_SUPPORTED;
        }
        change_value_binding = prev_type != item.item_type;
        prev_type = item.item_type;

        /* Bind the feature parameter. */
        ret = SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR,
                               SQL_VARCHAR, 0, 0, (SQLPOINTER)mv->value.key,
                               strlen(mv->value.key), NULL);
        if (!(SQL_SUCCEEDED(ret))) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            ERR_MSG_FMT_UNF("SQLBindParameter 2 returned: %d", ret);
            return ORCM_ERROR;
        }

        if (change_value_binding) {
            switch (item.item_type) {
            case ORCM_DB_ITEM_INTEGER:
                /* Value is integer, bind to the appropriate value. */
                ret = SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                                       SQL_BIGINT, 0, 0,
                                       (SQLPOINTER)&item.value.value_int,
                                       sizeof(item.value.value_int), NULL);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_UNF("SQLBindParameter 4 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the real value. */
                ret = SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_DOUBLE,
                                       SQL_DOUBLE, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_UNF("SQLBindParameter 5 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the string value. */
                ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_CHAR,
                                       SQL_VARCHAR, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_UNF("SQLBindParameter 6 returned: %d", ret);
                    return ORCM_ERROR;
                }
                break;
            case ORCM_DB_ITEM_REAL:
                /* Pass NULL for the integer value. */
                ret = SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                                       SQL_BIGINT, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_UNF("SQLBindParameter 4 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Value is real, bind to the appropriate value. */
                ret = SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_DOUBLE,
                                       SQL_DOUBLE, 0, 0,
                                       (SQLPOINTER)&item.value.value_real,
                                       sizeof(item.value.value_real), NULL);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_UNF("SQLBindParameter 5 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the string value. */
                ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_CHAR,
                                       SQL_VARCHAR, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_UNF("SQLBindParameter 6 returned: %d", ret);
                    return ORCM_ERROR;
                }
                break;
            case ORCM_DB_ITEM_STRING:
                /* Pass NULL for the integer value. */
                ret = SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                                       SQL_BIGINT, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_UNF("SQLBindParameter 4 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the real value. */
                ret = SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_DOUBLE,
                                       SQL_DOUBLE, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_UNF("SQLBindParameter 5 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Value is string, bind to the appropriate value. */
                ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_CHAR,
                                       SQL_VARCHAR, 0, 0,
                                       (SQLPOINTER)item.value.value_str,
                                       strlen(item.value.value_str), NULL);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_UNF("SQLBindParameter 6 returned: %d", ret);
                    return ORCM_ERROR;
                }
                break;
            default:
                SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                ERR_MSG_STORE("An unexpected error has occurred while "
                              "processing the values");
                return ORCM_ERROR;
            }
        } else if (ORCM_DB_ITEM_STRING == item.item_type) {
            /* No need to change the binding for all the values, just update
             * the string binding. */
            ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_CHAR,
                                   SQL_VARCHAR, 0, 0,
                                   (SQLPOINTER)item.value.value_str,
                                   strlen(item.value.value_str), NULL);
            if (!(SQL_SUCCEEDED(ret))) {
                SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                ERR_MSG_FMT_UNF("SQLBindParameter 6 returned: %d", ret);
                return ORCM_ERROR;
            }
        }

        if (NULL != mv->units) {
            /* Bind the units parameter. */
            ret = SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_CHAR,
                                   SQL_VARCHAR, 0, 0,
                                   (SQLPOINTER)mv->units,
                                   strlen(mv->units), NULL);
        } else {
            /* No units provided, bind NULL. */
            ret = SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_CHAR,
                                   SQL_VARCHAR, 0, 0, NULL, 0, &null_len);
        }
        if (!(SQL_SUCCEEDED(ret))) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            ERR_MSG_FMT_UNF("SQLBindParameter 7 returned: %d", ret);
            return ORCM_ERROR;
        }

        ret = SQLExecute(stmt);
        if (!(SQL_SUCCEEDED(ret))) {
            ERR_MSG_FMT_SQL_UNF(SQL_HANDLE_STMT, stmt,
                                "SQLExecute returned: %d", ret);
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            return ORCM_ERROR;
        }

        SQLCloseCursor(stmt);
    }

    opal_output_verbose(2, orcm_db_base_framework.framework_output,
                        "odbc_update_node_features succeeded");

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    return ORCM_SUCCESS;
}

#define ERR_MSG_RDT(msg) \
    opal_output(0, "***********************************************"); \
    opal_output(0, "db:odbc: Unable to record diagnostic test: "); \
    opal_output(0, msg); \
    opal_output(0, "***********************************************");

#define ERR_MSG_FMT_RDT(msg, ...) \
    opal_output(0, "***********************************************"); \
    opal_output(0, "db:odbc: Unable to record diagnostic test: "); \
    opal_output(0, msg, ##__VA_ARGS__); \
    opal_output(0, "***********************************************");

#define ERR_MSG_FMT_SQL_RDT(handle_type, handle, msg, ...) \
    opal_output(0, "***********************************************"); \
    opal_output(0, "db:odbc: Unable to record diagnostic test: "); \
    opal_output(0, msg, ##__VA_ARGS__); \
    opal_output(0, "ODBC error details:"); \
    odbc_error_info(handle_type, handle); \
    opal_output(0, "***********************************************");

static int odbc_record_diag_test(struct orcm_db_base_module_t *imod,
                                 const char *hostname,
                                 const char *diag_type,
                                 const char *diag_subtype,
                                 const struct tm *start_time,
                                 const struct tm *end_time,
                                 const int *component_index,
                                 const char *test_result,
                                 opal_list_t *test_params)
{
    mca_db_odbc_module_t *mod = (mca_db_odbc_module_t*)imod;
    orcm_metric_value_t *mv;

    SQL_TIMESTAMP_STRUCT start_time_sql;
    SQL_TIMESTAMP_STRUCT end_time_sql;

    orcm_db_item_t item;
    orcm_db_item_type_t prev_type = ORCM_DB_ITEM_INTEGER;
    int change_value_binding = 1;
    SQLLEN null_len = SQL_NULL_DATA;

    SQLRETURN ret;
    SQLHSTMT stmt;

    if (NULL == hostname) {
        ERR_MSG_RDT("No hostname provided");
        return ORCM_ERR_BAD_PARAM;
    }

    if (NULL == diag_type) {
        ERR_MSG_RDT("No diagnostic type provided");
        return ORCM_ERR_BAD_PARAM;
    }

    if (NULL == diag_subtype) {
        ERR_MSG_RDT("No diagnostic subtype provided");
        return ORCM_ERR_BAD_PARAM;
    }

    if (NULL == start_time) {
        ERR_MSG_RDT("No start time provided");
        return ORCM_ERR_BAD_PARAM;
    }

    if (NULL == test_result) {
        ERR_MSG_RDT("No test result provided");
        return ORCM_ERR_BAD_PARAM;
    }

    tm_to_sql_timestamp(&start_time_sql, start_time);

    if (NULL != end_time) {
        tm_to_sql_timestamp(&end_time_sql, end_time);
    }

    ret = SQLAllocHandle(SQL_HANDLE_STMT, mod->dbhandle, &stmt);
    if (!(SQL_SUCCEEDED(ret))) {
        ERR_MSG_FMT_RDT("SQLAllocHandle returned: %d", ret);
        return ORCM_ERROR;
    }

    /*
     * 1.- hostname
     * 2.- diagnostic type
     * 3.- diagnostic subtype
     * 4.- start time
     * 5.- end time
     * 6.- component index
     * 7.- test result
     */
    ret = SQLPrepare(stmt,
                     (SQLCHAR *)
                     "{call record_diag_test_result(?, ?, ?, ?, ?, ?, ?)}",
                     SQL_NTS);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_RDT("SQLPrepare returned: %d", ret);
        return ORCM_ERROR;
    }

    /* Bind hostname parameter. */
    ret = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, (SQLPOINTER)hostname, strlen(hostname), NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_RDT("SQLBindParameter 1 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind diagnostic type parameter. */
    ret = SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, (SQLPOINTER)diag_type, strlen(diag_type),
                           NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_RDT("SQLBindParameter 2 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind diagnostic subtype parameter. */
    ret = SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, (SQLPOINTER)diag_subtype, strlen(diag_subtype),
                           NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_RDT("SQLBindParameter 3 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind start time parameter. */
    ret = SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_TYPE_TIMESTAMP,
                           SQL_TYPE_TIMESTAMP, 0, 0,
                           (SQLPOINTER)&start_time_sql, sizeof(start_time_sql),
                           NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_RDT("SQLBindParameter 4 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind end time parameter. */
    if (NULL != end_time) {
        ret = SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_TYPE_TIMESTAMP,
                               SQL_TYPE_TIMESTAMP, 0, 0,
                               (SQLPOINTER)&end_time_sql, sizeof(end_time_sql),
                               NULL);
    } else {
        ret = SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_TYPE_TIMESTAMP,
                               SQL_TYPE_TIMESTAMP, 0, 0, NULL, 0, &null_len);
    }
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_RDT("SQLBindParameter 5 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind component index parameter. */
    if (NULL != component_index) {
        ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_LONG,
                               SQL_INTEGER, 0, 0, (SQLPOINTER)component_index,
                               sizeof(component_index), NULL);
    } else {
        ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_LONG,
                               SQL_INTEGER, 0, 0, NULL, 0, &null_len);
    }
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_RDT("SQLBindParameter 6 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind test result parameter. */
    ret = SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, (SQLPOINTER)test_result, strlen(test_result),
                           NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_RDT("SQLBindParameter 7 returned: %d", ret);
        return ORCM_ERROR;
    }

    ret = SQLExecute(stmt);
    if (!(SQL_SUCCEEDED(ret))) {
        ERR_MSG_FMT_SQL_RDT(SQL_HANDLE_STMT, stmt, "SQLExecute returned: %d",
                            ret);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return ORCM_ERROR;
    }

    SQLCloseCursor(stmt);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    if (NULL == test_params) {
        /* No test parameters provided, we're done! */
        opal_output_verbose(2, orcm_db_base_framework.framework_output,
                            "odbc_record_diag_test succeeded");
        return ORCM_SUCCESS;
    }

    ret = SQLAllocHandle(SQL_HANDLE_STMT, mod->dbhandle, &stmt);
    if (!(SQL_SUCCEEDED(ret))) {
        ERR_MSG_FMT_RDT("SQLAllocHandle returned: %d", ret);
        return ORCM_ERROR;
    }

    /*
     * 1.- hostname
     * 2.- diagnostic type
     * 3.- diagnostic subtype
     * 4.- start time
     * 5.- test parameter
     * 6.- data type
     * 7.- integer value
     * 8.- real value
     * 9.- string value
     * 10.- units
     */
    ret = SQLPrepare(stmt,
                     (SQLCHAR *)"{call record_diag_test_config"
                                "(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)}",
                     SQL_NTS);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_RDT("SQLPrepare returned: %d", ret);
        return ORCM_ERROR;
    }

    /* Bind hostname parameter. */
    ret = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, (SQLPOINTER)hostname, strlen(hostname), NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_RDT("SQLBindParameter 1 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind diagnostic type parameter. */
    ret = SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, (SQLPOINTER)diag_type, strlen(diag_type),
                           NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_RDT("SQLBindParameter 2 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind diagnostic subtype parameter. */
    ret = SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, (SQLPOINTER)diag_subtype, strlen(diag_subtype),
                           NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_RDT("SQLBindParameter 3 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind start time parameter. */
    ret = SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_TYPE_TIMESTAMP,
                           SQL_TYPE_TIMESTAMP, 0, 0,
                           (SQLPOINTER)&start_time_sql, sizeof(start_time_sql),
                           NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_RDT("SQLBindParameter 4 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind data type parameter. */
    ret = SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                           0, 0, (SQLPOINTER)&item.opal_type,
                           sizeof(item.opal_type), NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_UNF("SQLBindParameter 6 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind integer value parameter (assuming the value is integer for now). */
    ret = SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                           SQL_BIGINT, 0, 0, (SQLPOINTER)&item.value.value_int,
                           sizeof(item.value.value_int), NULL);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_UNF("SQLBindParameter 7 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind real value parameter (assuming NULL for now). */
    ret = SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE,
                           0, 0, NULL, 0, &null_len);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_UNF("SQLBindParameter 8 returned: %d", ret);
        return ORCM_ERROR;
    }
    /* Bind string value parameter (assuming NULL for now). */
    ret = SQLBindParameter(stmt, 9, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                           0, 0, NULL, 0, &null_len);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_UNF("SQLBindParameter 9 returned: %d", ret);
        return ORCM_ERROR;
    }

    OPAL_LIST_FOREACH(mv, test_params, orcm_metric_value_t) {
        if (NULL == mv->value.key || 0 == strlen(mv->value.key)) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            ERR_MSG_RDT("Key or test parameter name not provided for value");
            return ORCM_ERR_BAD_PARAM;
        }

        ret = opal_value_to_orcm_db_item(&mv->value, &item);
        if (ORCM_SUCCESS != ret) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            ERR_MSG_RDT("Unsupported value type");
            return ORCM_ERR_NOT_SUPPORTED;
        }
        change_value_binding = prev_type != item.item_type;
        prev_type = item.item_type;

        /* Bind the test parameter. */
        ret = SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_CHAR,
                               SQL_VARCHAR, 0, 0, (SQLPOINTER)mv->value.key,
                               strlen(mv->value.key), NULL);
        if (!(SQL_SUCCEEDED(ret))) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            ERR_MSG_FMT_RDT("SQLBindParameter 5 returned: %d", ret);
            return ORCM_ERROR;
        }

        if (NULL != mv->units) {
            /* Bind the units parameter. */
            ret = SQLBindParameter(stmt, 10, SQL_PARAM_INPUT, SQL_C_CHAR,
                                   SQL_VARCHAR, 0, 0,
                                   (SQLPOINTER)mv->units,
                                   strlen(mv->units), NULL);
        } else {
            /* No units provided, bind NULL. */
            ret = SQLBindParameter(stmt, 10, SQL_PARAM_INPUT, SQL_C_CHAR,
                                   SQL_VARCHAR, 0, 0, NULL, 0, &null_len);
        }
        if (!(SQL_SUCCEEDED(ret))) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            ERR_MSG_FMT_RDT("SQLBindParameter 10 returned: %d", ret);
            return ORCM_ERROR;
        }

        if (change_value_binding) {
            switch (item.item_type) {
            case ORCM_DB_ITEM_INTEGER:
                /* Value is integer, bind to the appropriate value. */
                ret = SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                                       SQL_BIGINT, 0, 0,
                                       (SQLPOINTER)&item.value.value_int,
                                       sizeof(item.value.value_int), NULL);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_RDT("SQLBindParameter 7 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the real value. */
                ret = SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_DOUBLE,
                                       SQL_DOUBLE, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_RDT("SQLBindParameter 8 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the string value. */
                ret = SQLBindParameter(stmt, 9, SQL_PARAM_INPUT, SQL_C_CHAR,
                                       SQL_VARCHAR, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_RDT("SQLBindParameter 9 returned: %d", ret);
                    return ORCM_ERROR;
                }
                break;
            case ORCM_DB_ITEM_REAL:
                /* Pass NULL for the integer value. */
                ret = SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                                       SQL_BIGINT, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_RDT("SQLBindParameter 7 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Value is real, bind to the appropriate value. */
                ret = SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_DOUBLE,
                                       SQL_DOUBLE, 0, 0,
                                       (SQLPOINTER)&item.value.value_real,
                                       sizeof(item.value.value_real), NULL);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_RDT("SQLBindParameter 8 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the string value. */
                ret = SQLBindParameter(stmt, 9, SQL_PARAM_INPUT, SQL_C_CHAR,
                                       SQL_VARCHAR, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_RDT("SQLBindParameter 9 returned: %d", ret);
                    return ORCM_ERROR;
                }
                break;
            case ORCM_DB_ITEM_STRING:
                /* Pass NULL for the integer value. */
                ret = SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                                       SQL_BIGINT, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_RDT("SQLBindParameter 7 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Pass NULL for the real value. */
                ret = SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_DOUBLE,
                                       SQL_DOUBLE, 0, 0, NULL,
                                       0, &null_len);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_RDT("SQLBindParameter 8 returned: %d", ret);
                    return ORCM_ERROR;
                }
                /* Value is string, bind to the appropriate value. */
                ret = SQLBindParameter(stmt, 9, SQL_PARAM_INPUT, SQL_C_CHAR,
                                       SQL_VARCHAR, 0, 0,
                                       (SQLPOINTER)item.value.value_str,
                                       strlen(item.value.value_str), NULL);
                if (!(SQL_SUCCEEDED(ret))) {
                    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                    ERR_MSG_FMT_RDT("SQLBindParameter 9 returned: %d", ret);
                    return ORCM_ERROR;
                }
                break;
            default:
                SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                ERR_MSG_RDT("An unexpected error has occurred while "
                            "processing the values");
                return ORCM_ERROR;
            }
        } else if (ORCM_DB_ITEM_STRING == item.item_type) {
            /* No need to change the binding for all the values, just update
             * the string binding. */
            ret = SQLBindParameter(stmt, 9, SQL_PARAM_INPUT, SQL_C_CHAR,
                                   SQL_VARCHAR, 0, 0,
                                   (SQLPOINTER)item.value.value_str,
                                   strlen(item.value.value_str), NULL);
            if (!(SQL_SUCCEEDED(ret))) {
                SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                ERR_MSG_FMT_RDT("SQLBindParameter 9 returned: %d", ret);
                return ORCM_ERROR;
            }
        }

        ret = SQLExecute(stmt);
        if (!(SQL_SUCCEEDED(ret))) {
            ERR_MSG_FMT_SQL_RDT(SQL_HANDLE_STMT, stmt,
                                  "SQLExecute returned: %d", ret);
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            return ORCM_ERROR;
        }

        SQLCloseCursor(stmt);
    }

    opal_output_verbose(2, orcm_db_base_framework.framework_output,
                        "odbc_record_diag_test succeeded");

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    return ORCM_SUCCESS;
}

#define ERR_MSG_FMT_FETCH(msg, ...) \
    opal_output(0, "***********************************************"); \
    opal_output(0, "db:odbc: Unable to fetch data: "); \
    opal_output(0, msg, ##__VA_ARGS__); \
    opal_output(0, "***********************************************");

static int odbc_fetch(struct orcm_db_base_module_t *imod,
                      const char *primary_key,
                      const char *key,
                      opal_list_t *kvs)
{
    mca_db_odbc_module_t *mod = (mca_db_odbc_module_t*)imod;

    SQLRETURN ret;
    SQLHSTMT stmt;
    SQLSMALLINT cols;
    SQLSMALLINT type;
    SQLULEN len;
    char query[1024];
    opal_value_t *kv;
    SQL_TIMESTAMP_STRUCT time_stamp;
    struct tm temp_tm;
    SQLUSMALLINT i;

    snprintf(query, sizeof(query), "select * from %s where %s",
             mod->table, primary_key);

    ret = SQLAllocHandle(SQL_HANDLE_STMT, mod->dbhandle, &stmt);
    if (!(SQL_SUCCEEDED(ret))) {
        ERR_MSG_FMT_FETCH("SQLAllocHandle returned: %d", ret);
        return ORCM_ERROR;
    }

    ret = SQLExecDirect(stmt, (SQLCHAR *)query, SQL_NTS);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_FETCH("SQLExecDirect returned: %d", ret);
        return ORCM_ERROR;
    }

    ret = SQLNumResultCols(stmt, &cols);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_FETCH("SQLNumResultCols returned: %d", ret);
        return ORCM_ERROR;
    }

    ret = SQLFetch(stmt);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_FETCH("SQLFetch returned: %d", ret);
        return ORCM_ERROR;
    }

    for (i = 1; i <= cols; i++) {
        ret = SQLDescribeCol(stmt, i, NULL, 0, NULL, &type, &len, NULL, NULL);
        if (!(SQL_SUCCEEDED(ret))) {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            ERR_MSG_FMT_FETCH("SQLDescribeCol returned: %d", ret);
            return ORCM_ERROR;
        }

        kv = OBJ_NEW(opal_value_t);
        switch (type) {
            case SQL_CHAR:
            case SQL_VARCHAR:
                kv->type = OPAL_STRING;
                kv->data.string = (char *)malloc(len);
                ret = SQLGetData(stmt, i, SQL_C_CHAR, kv->data.string,
                                 len, NULL);
                break;
            case SQL_DECIMAL:
            case SQL_NUMERIC:
            case SQL_REAL:
            case SQL_FLOAT:
                kv->type = OPAL_FLOAT;
                ret = SQLGetData(stmt, i, SQL_C_FLOAT, &kv->data.fval,
                                 sizeof(kv->data.fval), NULL);
                break;
            case SQL_DOUBLE:
                kv->type = OPAL_DOUBLE;
                ret = SQLGetData(stmt, i, SQL_C_DOUBLE, &kv->data.dval,
                                 sizeof(kv->data.dval), NULL);
                break;
            case SQL_SMALLINT:
                kv->type = OPAL_INT16;
                ret = SQLGetData(stmt, i, SQL_C_SSHORT, &kv->data.int16,
                                 sizeof(kv->data.int16), NULL);
                break;
            case SQL_INTEGER:
                kv->type = OPAL_INT32;
                ret = SQLGetData(stmt, i, SQL_C_SLONG, &kv->data.int32,
                                 sizeof(kv->data.int32), NULL);
                break;
            case SQL_BIT:
            case SQL_TINYINT:
                kv->type = OPAL_BYTE;
                ret = SQLGetData(stmt, i, SQL_C_UTINYINT, &kv->data.byte,
                                 sizeof(kv->data.byte), NULL);
                break;
            /* TODO: add support for dates and times */
            /*case SQL_TYPE_DATE:
            case SQL_TYPE_TIME:*/
            case SQL_TYPE_TIMESTAMP:
                kv->type = OPAL_TIMEVAL;
                ret = SQLGetData(stmt, i, SQL_C_TYPE_TIMESTAMP, &time_stamp,
                                 sizeof(time_stamp), NULL);
                /* The year in tm represents the number of years since 1900 */
                temp_tm.tm_year = time_stamp.year - 1900;
                /* The month in tm is zero-based */
                memset(&temp_tm, 0, sizeof(temp_tm));
                temp_tm.tm_mon = time_stamp.month - 1;
                temp_tm.tm_mday = time_stamp.day;
                temp_tm.tm_hour = time_stamp.hour;
                temp_tm.tm_min = time_stamp.minute;
                temp_tm.tm_sec = time_stamp.second;

                kv->data.tv.tv_sec = mktime(&temp_tm);
                kv->data.tv.tv_usec = 0;
                break;
            default:
                /* TODO: unsupported type (ignore for now) */
                continue;
        }
        if (!(SQL_SUCCEEDED(ret))) {
            OBJ_RELEASE(kv);
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            ERR_MSG_FMT_FETCH("SQLGetData returned: %d", ret);
            return ORCM_ERROR;
        }

        opal_list_append(kvs, &kv->super);
    }
    
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    
    return ORCM_SUCCESS;
}

#define ERR_MSG_FMT_REMOVE(msg, ...) \
    opal_output(0, "***********************************************"); \
    opal_output(0, "db:odbc: Unable to remove data: "); \
    opal_output(0, msg, ##__VA_ARGS__); \
    opal_output(0, "***********************************************");

static int odbc_remove(struct orcm_db_base_module_t *imod,
                       const char *primary_key,
                       const char *key)
{
    mca_db_odbc_module_t *mod = (mca_db_odbc_module_t*)imod;

    SQLRETURN ret;
    SQLHSTMT stmt;
    char query[1024];

    snprintf(query, sizeof(query), "delete from %s where %s",
             mod->table, primary_key);

    ret = SQLAllocHandle(SQL_HANDLE_STMT, mod->dbhandle, &stmt);
    if (!(SQL_SUCCEEDED(ret))) {
        ERR_MSG_FMT_REMOVE("SQLAllocHandle returned: %d", ret);
        return ORCM_ERROR;
    }

    ret = SQLExecDirect(stmt, (SQLCHAR *)query, SQL_NTS);
    if (!(SQL_SUCCEEDED(ret))) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        ERR_MSG_FMT_REMOVE("SQLExecDirect returned: %d", ret);
        return ORCM_ERROR;
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    return ORCM_SUCCESS;
}

static void odbc_error_info(SQLSMALLINT handle_type, SQLHANDLE handle)
{
    int i = 1;
    int ret;
    SQLCHAR sql_state[6];
    SQLINTEGER native_error;
    SQLCHAR msg[SQL_MAX_MESSAGE_LENGTH];
    SQLSMALLINT msg_len;

    ret = SQLGetDiagRec(handle_type, handle, i, sql_state, &native_error, msg,
                        sizeof(msg), &msg_len);
    if (!(SQL_SUCCEEDED(ret))) {
        opal_output(0, "Unable to retrieve ODBC error information");
        return;
    }

    do {
        opal_output(0, "Status record: %d", i);
        opal_output(0, "SQL state: %s", sql_state);
        opal_output(0, "Native error: %d", native_error);
        opal_output(0, "Message: %s", msg);

        i++;
        ret = SQLGetDiagRec(handle_type, handle, i, sql_state, &native_error,
                            msg, sizeof(msg), &msg_len);
    } while (SQL_SUCCEEDED(ret));
}

static void tm_to_sql_timestamp(SQL_TIMESTAMP_STRUCT *sql_timestamp,
                             const struct tm *time_info)
{
    sql_timestamp->year = time_info->tm_year + 1900;
    sql_timestamp->month = time_info->tm_mon + 1;
    sql_timestamp->day = time_info->tm_mday;
    sql_timestamp->hour = time_info->tm_hour;
    sql_timestamp->minute = time_info->tm_min;
    sql_timestamp->second = time_info->tm_sec;
    sql_timestamp->fraction = 0;
}

static void tv_to_sql_timestamp(SQL_TIMESTAMP_STRUCT *sql_timestamp,
                             const struct timeval *time)
{
    struct tm time_info = *localtime(&time->tv_sec);

    tm_to_sql_timestamp(sql_timestamp, &time_info);
    sql_timestamp->fraction = time->tv_usec * 1000;
}
