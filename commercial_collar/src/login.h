/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#ifndef _LOGIN_H_
#define _LOGIN_H_

#include <stdbool.h>

/**
 * Initialize login subsys
 */
void login_init(void);

/**
 * log out current user and switch to login shell
 */
void logout(void);

/**
 * disable the nrf5340 console completely
 * @return false if the override gpio is set, otherwise true if the console has been disabled
 */
bool disable_console(void);
#endif
