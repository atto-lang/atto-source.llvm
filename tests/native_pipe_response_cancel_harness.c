#include "oreslang/runtime/SourceActorBootstrap.h"
#include <stdio.h>

/* Inject cancellation after the DATA handler computes its scalar snapshot,
 * immediately before response admission. This never runs in production. */
void oreslang_source_actor_test_before_pipeline_response(void* source) {
    oreslang_source_actor_pipeline_cancel(source);
    fputs("PIPE_RESPONSE_CANCEL_INJECTED_BEFORE_COMMIT\n", stderr);
}
