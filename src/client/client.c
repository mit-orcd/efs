#include "client_internal.h"

struct efs_client g_client = {
    .conn_fd = {-1, -1, -1},
};
