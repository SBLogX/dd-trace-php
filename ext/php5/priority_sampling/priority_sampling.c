#include "priority_sampling.h"

#include <mt19937-64.h>

#include <ext/pcre/php_pcre.h>
#include <ext/standard/base64.h>

#include "../configuration.h"
#include "../span.h"

ZEND_EXTERN_MODULE_GLOBALS(ddtrace);

enum dd_sampling_mechanism {
    DD_MECHANISM_AGENT_RATE = 1,
    DD_MECHANISM_REMOTE_RATE = 2,
    DD_MECHANISM_RULE = 3,
    DD_MECHANISM_MANUAL = 4,
};

static void dd_update_upstream_services(ddtrace_span_fci *span, ddtrace_span_fci *deciding_span,
                                        enum dd_sampling_mechanism mechanism TSRMLS_DC) {
    zval *meta = ddtrace_spandata_property_meta(&span->span);
    if (Z_TYPE_P(meta) != IS_ARRAY) {
        zval_ptr_dtor(&meta);
        array_init(meta);
    }

    zval **current_services_zv = NULL;

    zend_hash_find(&DDTRACE_G(root_span_tags_preset), "_dd.p.upstream_services", sizeof("_dd.p.upstream_services"),
                   (void **)&current_services_zv);

    char *current_services = current_services_zv ? Z_STRVAL_PP(current_services_zv) : "";

    long sampling_priority = ddtrace_fetch_prioritySampling_from_root(TSRMLS_C);
    if (DDTRACE_G(propagated_priority_sampling) == sampling_priority ||
        sampling_priority == DDTRACE_PRIORITY_SAMPLING_UNSET) {
        if (strlen(current_services)) {
            add_assoc_string(meta, "_dd.p.upstream_services", current_services, 1);
        } else {
            zend_hash_del(Z_ARRVAL_P(meta), "_dd.p.upstream_services", sizeof("_dd.p.upstream_services"));
        }
        return;
    }

    zval *service = ddtrace_spandata_property_service(&deciding_span->span);

    int b64_servicename_length = 0;
    unsigned char *b64_servicename =
        php_base64_encode((unsigned char *)Z_STRVAL_P(service), Z_STRLEN_P(service), &b64_servicename_length);
    while (b64_servicename_length > 0 && b64_servicename[b64_servicename_length - 1] == '=') {
        b64_servicename[--b64_servicename_length] = 0;  // remove padding
    }

    char sampling_rate[7] = {0};
    zval *metrics = ddtrace_spandata_property_metrics(&span->span), **sample_rate;
    if (Z_TYPE_P(metrics) == IS_ARRAY && (zend_hash_find(Z_ARRVAL_P(metrics), "_dd.rule_psr", sizeof("_dd.rule_psr"),
                                                         (void **)&sample_rate) == SUCCESS)) {
        snprintf(sampling_rate, 6, "%f", Z_DVAL_PP(sample_rate));
    }

    char *new_services;
    spprintf(&new_services, 0, "%s%s%s|%d|%d|%s",
             current_services_zv && Z_TYPE_PP(current_services_zv) == IS_STRING ? Z_STRVAL_PP(current_services_zv) : "",
             current_services_zv && (Z_TYPE_PP(current_services_zv) == IS_STRING) && Z_STRLEN_PP(current_services_zv)
                 ? ";"
                 : "",
             b64_servicename, (int)sampling_priority, mechanism, sampling_rate);
    add_assoc_string(meta, "_dd.p.upstream_services", new_services, 0);

    efree(b64_servicename);
}

static bool dd_rule_matches(zval *pattern, zval *prop TSRMLS_DC) {
    if (Z_TYPE_P(pattern) != IS_STRING) {
        return false;
    }
    if (Z_TYPE_P(prop) != IS_STRING) {
        return true;  // default case unset or null must be true, everything else is too then...
    }

    char *regex;
    int regexlen = spprintf(&regex, 0, "(%s)", Z_STRVAL_P(pattern));
    pcre_cache_entry *pce = pcre_get_compiled_regex_cache(regex, regexlen TSRMLS_CC);
    zval ret;
    php_pcre_match_impl(pce, Z_STRVAL_P(prop), Z_STRLEN_P(prop), &ret, NULL, 0, 0, 0, 0 TSRMLS_CC);
    efree(regex);
    return Z_TYPE(ret) == IS_LONG && Z_LVAL(ret) > 0;
}

static void dd_decide_on_sampling(ddtrace_span_fci *span TSRMLS_DC) {
    int priority = DDTRACE_G(default_priority_sampling);
    enum dd_sampling_mechanism mechanism = DD_MECHANISM_MANUAL;
    if (priority == DDTRACE_PRIORITY_SAMPLING_UNKNOWN) {
        double sample_rate = get_DD_TRACE_SAMPLE_RATE();
        bool explicit_rule = zai_config_memoized_entries[DDTRACE_CONFIG_DD_TRACE_SAMPLE_RATE].name_index >= 0;
        HashPosition pos;
        HashTable *rules = get_DD_TRACE_SAMPLING_RULES();
        zval **rule;
        for (zend_hash_internal_pointer_reset_ex(rules, &pos);
             zend_hash_get_current_data_ex(rules, (void **)&rule, &pos) == SUCCESS;
             zend_hash_move_forward_ex(rules, &pos)) {
            if (Z_TYPE_PP(rule) != IS_ARRAY) {
                continue;
            }

            bool rule_matches = true;

            zval **rule_pattern;
            if (zend_hash_find(Z_ARRVAL_PP(rule), "service", sizeof("service"), (void **)&rule_pattern) == SUCCESS) {
                rule_matches &=
                    dd_rule_matches(*rule_pattern, ddtrace_spandata_property_service(&span->span) TSRMLS_CC);
            }
            if (zend_hash_find(Z_ARRVAL_PP(rule), "name", sizeof("name"), (void **)&rule_pattern) == SUCCESS) {
                rule_matches &= dd_rule_matches(*rule_pattern, ddtrace_spandata_property_name(&span->span) TSRMLS_CC);
            }

            zval **sample_rate_zv;
            if (rule_matches && zend_hash_find(Z_ARRVAL_PP(rule), "sample_rate", sizeof("sample_rate"),
                                               (void **)&sample_rate_zv) == SUCCESS) {
                zval doublezv = **sample_rate_zv;
                zval_copy_ctor(&doublezv);
                convert_to_double(&doublezv);
                sample_rate = Z_DVAL(doublezv);
                explicit_rule = true;
                break;
            }
        }

        add_assoc_double_ex(ddtrace_spandata_property_metrics(&span->span), "_dd.rule_psr", sizeof("_dd.rule_psr"),
                            sample_rate);

        bool sampling = (double)genrand64_int64() < sample_rate * (double)~0ULL;

        if (explicit_rule) {
            mechanism = DD_MECHANISM_RULE;
            priority = sampling ? PRIORITY_SAMPLING_USER_KEEP : PRIORITY_SAMPLING_USER_REJECT;
        } else {
            mechanism = DD_MECHANISM_AGENT_RATE;
            priority = sampling ? PRIORITY_SAMPLING_AUTO_KEEP : PRIORITY_SAMPLING_AUTO_REJECT;
        }
    }
    add_assoc_long_ex(ddtrace_spandata_property_metrics(&span->span), "_sampling_priority_v1",
                      sizeof("_sampling_priority_v1"), priority);

    dd_update_upstream_services(span, span, mechanism TSRMLS_CC);
}

long ddtrace_fetch_prioritySampling_from_root(TSRMLS_D) {
    zval **priority_zv;
    ddtrace_span_fci *root_span = DDTRACE_G(root_span);

    if (!root_span) {
        if (DDTRACE_G(default_priority_sampling) == DDTRACE_PRIORITY_SAMPLING_UNSET) {
            return DDTRACE_PRIORITY_SAMPLING_UNKNOWN;
        }
        return DDTRACE_G(default_priority_sampling);
    }

    HashTable *root_metrics = Z_ARRVAL_P(ddtrace_spandata_property_metrics(&root_span->span));

    if (zend_hash_find(root_metrics, "_sampling_priority_v1", sizeof("_sampling_priority_v1"), (void **)&priority_zv) ==
        FAILURE) {
        if (DDTRACE_G(default_priority_sampling) == DDTRACE_PRIORITY_SAMPLING_UNSET) {
            return DDTRACE_PRIORITY_SAMPLING_UNKNOWN;
        }

        dd_decide_on_sampling(root_span TSRMLS_CC);
        zend_hash_find(root_metrics, "_sampling_priority_v1", sizeof("_sampling_priority_v1"), (void **)&priority_zv);
    }

    convert_to_long(*priority_zv);
    return Z_LVAL_PP(priority_zv);
}

void ddtrace_set_prioritySampling_on_root(long priority TSRMLS_DC) {
    ddtrace_span_fci *root_span = DDTRACE_G(root_span);

    if (!root_span) {
        return;
    }

    HashTable *root_metrics = Z_ARRVAL_P(ddtrace_spandata_property_metrics(&root_span->span));

    if (priority == DDTRACE_PRIORITY_SAMPLING_UNKNOWN || priority == DDTRACE_PRIORITY_SAMPLING_UNSET) {
        zend_hash_del(root_metrics, "_sampling_priority_v1", sizeof("_sampling_priority_v1"));
    } else {
        zval *zv;
        MAKE_STD_ZVAL(zv);
        ZVAL_LONG(zv, priority);
        zend_hash_update(root_metrics, "_sampling_priority_v1", sizeof("_sampling_priority_v1"), &zv, sizeof(zval *),
                         NULL);

        dd_update_upstream_services(root_span, DDTRACE_G(open_spans_top), DD_MECHANISM_MANUAL TSRMLS_CC);
    }
}
