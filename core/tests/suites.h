/*
 * suites.h - the test suites, one per file, run from tests/test_core.c.
 *
 * Split up because the Phase 1 parser tests are larger than everything Phase 0
 * needed put together, and because the property tests need the generated
 * fixtures while the unit tests deliberately do not.
 */
#ifndef IBHA_TEST_SUITES_H
#define IBHA_TEST_SUITES_H

#include "../include/ibha_csvdiff.h"

void ibha_test_hash(void);
void ibha_test_parse(void);
void ibha_test_schema(void);
void ibha_test_normalize(void);
void ibha_test_match(void);
void ibha_test_diff(void);
void ibha_test_validate(void);
void ibha_test_columns(void);
void ibha_test_segment(void);
void ibha_test_emit(const char *fixture_dir);
void ibha_test_property(const char *fixture_dir);

/* Byte identical comparison of two indexes, including the row digests. Used by
 * the chunking property and by the zero copy path's assertion that it agrees
 * with the streamed one. */
int ibha_tables_identical(const ibha_csvd_table *a, const ibha_csvd_table *b);

#endif /* IBHA_TEST_SUITES_H */
