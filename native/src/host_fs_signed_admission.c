#include "atto/host_fs_signed_admission.h"
#include <errno.h>
#include <openssl/crypto.h>
#include <string.h>

static const char *scope_or_null(const char *text) {
    return text && text[0] ? text : NULL;
}

int atto_fs_broker_admit_signed(atto_fs_broker *broker,
                                const atto_fs_signed_launch *launch) {
    if (!broker || !launch || !launch->trusted_root ||
        !launch->artifact_bytes || !launch->artifact_size ||
        !launch->signed_descriptor || !launch->signed_descriptor_size ||
        !launch->signature_ed25519 || !launch->trusted_public_key ||
        !launch->tenant_id || !launch->generation) return -EINVAL;

    /* The signed maximum is independently authenticated and bound to the
     * exact executable-image bytes BEFORE the broker can be initialized.
     * Never copy the artifact ceiling from an unverified launch field. */
    atto_fs_verified_ceiling ceiling = {0};
    const int verify = atto_fs_verify_signed_ceiling_v1(
        launch->artifact_bytes, launch->artifact_size,
        launch->signed_descriptor, launch->signed_descriptor_size,
        launch->signature_ed25519, launch->trusted_public_key, &ceiling);
    if (verify != 0) {
        OPENSSL_cleanse(&ceiling, sizeof(ceiling));
        return -EACCES;
    }

    const atto_fs_layered_policy policy = {
        .tenant_id = launch->tenant_id,
        .generation = launch->generation,
        .host = launch->host,
        .artifact = {
            scope_or_null(ceiling.allow_read),
            scope_or_null(ceiling.allow_write),
            scope_or_null(ceiling.deny_read),
            scope_or_null(ceiling.deny_write)
        },
        .runtime = launch->runtime,
        .tenant = launch->tenant,
        .actor = launch->actor,
        .delegation = launch->delegation
    };
    /* broker_init_layers makes its own copies of the scopes before return.
     * The verified temporary can therefore be zeroed without weakening the
     * broker's permanent allow/deny snapshot. */
    const int admitted = atto_fs_broker_init_layers(broker, launch->trusted_root, &policy);
    OPENSSL_cleanse(&ceiling, sizeof(ceiling));
    return admitted;
}
