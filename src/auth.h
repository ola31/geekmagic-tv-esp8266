#ifndef AUTH_H
#define AUTH_H

#include <Arduino.h>

#define AUTH_CONFIG_PATH "/auth.json"
#define AUTH_CONFIG_VERSION 2
#define AUTH_MIN_PASSWORD_LENGTH 4
#define AUTH_MAX_PASSWORD_LENGTH 32

void authInit();
bool authVerifyPassword(const String &password);
bool authUpdatePassword(const String &currentPassword, const String &newPassword, String *error = nullptr);
bool authValidatePasswordPolicy(const String &password, String *error = nullptr);
const char* authUsername();
const char* authOtaPasswordHash();
bool authWasProvisionedThisBoot();
bool authCanRevealPassword();
const char* authProvisionedPassword();

#endif
