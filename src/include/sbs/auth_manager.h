#ifndef SBS_AUTH_MANAGER_H
#define SBS_AUTH_MANAGER_H

#include "sbs/types.h"

#include <stdbool.h>

typedef struct cJSON cJSON;
typedef struct sbs_auth_manager sbs_auth_manager_t;

sbs_auth_manager_t *sbs_auth_manager_new(const char *config_dir);
void sbs_auth_manager_free(sbs_auth_manager_t *auth);

bool sbs_auth_manager_passwordless(const sbs_auth_manager_t *auth);
bool sbs_auth_manager_setup_required(const sbs_auth_manager_t *auth);
cJSON *sbs_auth_manager_status_json(const sbs_auth_manager_t *auth);

int sbs_auth_manager_setup(sbs_auth_manager_t *auth,
                           const char *username,
                           const char *password,
                           char **session_token_out);
int sbs_auth_manager_login_password(sbs_auth_manager_t *auth,
                                    const char *username,
                                    const char *password,
                                    char **session_token_out);
int sbs_auth_manager_login_api_key(sbs_auth_manager_t *auth,
                                   const char *api_key,
                                   char **session_token_out);
bool sbs_auth_manager_validate_session(sbs_auth_manager_t *auth,
                                       const char *session_token);
bool sbs_auth_manager_validate_api_key(sbs_auth_manager_t *auth,
                                       const char *api_key);
int sbs_auth_manager_create_api_key(sbs_auth_manager_t *auth,
                                    const char *name,
                                    char **api_key_out,
                                    cJSON **metadata_out);
int sbs_auth_manager_list_api_keys(sbs_auth_manager_t *auth,
                                   cJSON **api_keys_out);
int sbs_auth_manager_delete_api_key(sbs_auth_manager_t *auth,
                                    const char *id);
int sbs_auth_manager_update_credentials(sbs_auth_manager_t *auth,
                                        const char *username,
                                        const char *password,
                                        char **session_token_out);
int sbs_auth_manager_set_passwordless(sbs_auth_manager_t *auth,
                                      bool passwordless);

#endif
