/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: 84c4b38fe2cdc3ed1bbe05cf972cd1246b99e424 */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cbox_telemetry_version, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cbox_telemetry_status, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cbox_telemetry_begin, 0, 0, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, context, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cbox_telemetry_finish, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, handle, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, includeProfile, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, includeStacks, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cbox_telemetry_drain_crashes, 0, 0, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, max, IS_LONG, 0, "32")
ZEND_END_ARG_INFO()

ZEND_FUNCTION(cbox_telemetry_version);
ZEND_FUNCTION(cbox_telemetry_status);
ZEND_FUNCTION(cbox_telemetry_begin);
ZEND_FUNCTION(cbox_telemetry_finish);
ZEND_FUNCTION(cbox_telemetry_drain_crashes);

static const zend_function_entry ext_functions[] = {
	ZEND_FE(cbox_telemetry_version, arginfo_cbox_telemetry_version)
	ZEND_FE(cbox_telemetry_status, arginfo_cbox_telemetry_status)
	ZEND_FE(cbox_telemetry_begin, arginfo_cbox_telemetry_begin)
	ZEND_FE(cbox_telemetry_finish, arginfo_cbox_telemetry_finish)
	ZEND_FE(cbox_telemetry_drain_crashes, arginfo_cbox_telemetry_drain_crashes)
	ZEND_FE_END
};
