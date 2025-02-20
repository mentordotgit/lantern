#include <postgres.h>

#include "utils.h"

#include <assert.h>
#include <catalog/pg_type_d.h>
#include <executor/spi.h>
#include <math.h>
#include <miscadmin.h>
#include <regex.h>
#include <string.h>
#include <utils/builtins.h>

#if PG_VERSION_NUM >= 130000
#include <utils/memutils.h>
#endif

#include "external_index.h"
#include "hnsw.h"
#include "options.h"
#include "usearch.h"
#include "version.h"

bool versions_match = false;
bool version_checked = false;

void LogUsearchOptions(usearch_init_options_t *opts)
{
    /*todo:: in usearch.h create const char arrays like
char* scalar_names = {
    usearch_scalar_f32_k: "f32",
    usearch_scalar_f64_k: "f64"
}
so below the human readable string names can be printed
*/
    elog(INFO,
         "usearch_init_options_t: metric_kind: %d, metric: %p, "
         "quantization: %d, dimensions: %ld, connectivity: %ld, "
         "expansion_add: %ld, expansion_search: %ld",
         opts->metric_kind,
         opts->metric,
         opts->quantization,
         opts->dimensions,
         opts->connectivity,
         opts->expansion_add,
         opts->expansion_search);
}

void PopulateUsearchOpts(Relation index, usearch_init_options_t *opts)
{
    opts->connectivity = ldb_HnswGetM(index);
    opts->expansion_add = ldb_HnswGetEfConstruction(index);
    opts->expansion_search = ldb_HnswGetEf(index);
    opts->metric_kind = ldb_HnswGetMetricKind(index);
    opts->metric = NULL;
    opts->quantization = usearch_scalar_f32_k;
}

usearch_label_t GetUsearchLabel(ItemPointer itemPtr)
{
    usearch_label_t label = 0;
    memcpy((unsigned long *)&label, itemPtr, 6);
    return label;
}

void CheckMem(int limit, Relation index, usearch_index_t uidx, uint32 n_nodes, char *msg)
{
    uint32 node_size = 0;
    if(index != NULL) {
        usearch_error_t    error;
        double             M = ldb_HnswGetM(index);
        double             mL = 1 / log(M);
        usearch_metadata_t meta = usearch_metadata(uidx, &error);
        // todo:: update sizeof(float) to correct vector size once #19 is merged
        node_size = UsearchNodeBytes(&meta, meta.dimensions * sizeof(float), (int)round(mL + 1));
    }
    // todo:: there's figure out a way to check this in pg <= 12
#if PG_VERSION_NUM >= 130000
    Size pg_mem = MemoryContextMemAllocated(CurrentMemoryContext, true);
#else
    Size pg_mem = 0;
#endif

    // The average number of layers for an element to be added in is mL+1 per section 4.2.2
    // Accuracy could maybe be improved by not rounding
    // This is a guess, but it's a reasonably good one
    if(pg_mem + node_size * n_nodes > (uint32)limit * 1024UL) {
        elog(WARNING, "%s", msg);
    }
}

// if the element type of the passed array is already float4, this function just returns that pointer
// otherwise, it allocates a new array, casts all elements to float4 and returns the resulting array
float4 *ToFloat4Array(ArrayType *arr)
{
    Oid element_type = ARR_ELEMTYPE(arr);
    if(element_type == FLOAT4OID) {
        return (float4 *)ARR_DATA_PTR(arr);
    } else if(element_type == INT4OID) {
        int arr_dim = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));

        float4 *result = palloc(arr_dim * sizeof(int32));
        int32  *typed_src = (int32 *)ARR_DATA_PTR(arr);
        for(int i = 0; i < arr_dim; i++) {
            result[ i ] = typed_src[ i ];
        }
        return result;
    } else {
        elog(ERROR, "unsupported element type: %d", element_type);
    }
}

// Check if the binary version matches the schema version caching the result after the first check
// This is used to prevent interacting with the index when the two don't match
bool VersionsMatch()
{
    // If a parallel worker runs as a result of query execution, executing the SQL query below will lead to the
    // error "ERROR:  cannot execute SQL without an outer snapshot or portal." Instead of loading in a snapshot, we
    // simply return if one doesn't exist, the idea being that in the case of a parallel worker running this
    // function, the original worker will have already run this function (after which all the parallel workers run
    // this function, invoked by _PG_init). We return true so that we suppress any version mismatch messages from
    // callers of this function
    if(!ActiveSnapshotSet()) {
        version_checked = versions_match = false;
        return true;
    }

    if(likely(version_checked)) {
        return versions_match;
    } else {
        const char *query;
        const char *version;
        bool        isnull;
        int         version_length;
        int         spi_result;
        int         comparison;
        Datum       val;
        text       *version_text;

        if(SPI_connect() != SPI_OK_CONNECT) {
            elog(ERROR, "could not connect to executor to check binary version");
        }

        query = "SELECT extversion FROM pg_extension WHERE extname = 'lantern'";

        // Execute the query to figure out what version of lantern is in use in SQL
        spi_result = SPI_execute(query, true, 0);
        if(spi_result != SPI_OK_SELECT) {
            elog(ERROR, "SPI_execute returned %s for %s", SPI_result_code_string(spi_result), query);
        }

        // Global containing the number of rows processed, should be just 1
        if(SPI_processed != 1) {
            elog(ERROR, "SQL version query did not return any values");
        }

        // SPI_tuptable is a global populated by SPI_execute
        val = SPI_getbinval(SPI_tuptable->vals[ 0 ], SPI_tuptable->tupdesc, 1, &isnull);

        if(isnull) {
            elog(ERROR, "Version query returned null");
        }

        // Grab the result and check that it matches the version in the generated header
        version_text = DatumGetTextP(val);
        version = text_to_cstring(version_text);
        version_length = strlen(version);
        if(sizeof(LDB_BINARY_VERSION) >= (unsigned)version_length) {
            version_length = sizeof(LDB_BINARY_VERSION);
        }

        comparison = strncmp(version, LDB_BINARY_VERSION, version_length);

        if(comparison == 0) {
            versions_match = true;
        }
        version_checked = true;

        SPI_finish();

        if(!versions_match) {
            elog(WARNING,
                 "LanternDB binary version (%s) does not match the version in SQL (%s). This can cause errors as the "
                 "two "
                 "APIs may "
                 "differ. Please run `ALTER EXTENSION lantern UPDATE` and reconnect before attempting to work with "
                 "indices",
                 LDB_BINARY_VERSION,
                 version);
        }
        return versions_match;
    }
}
