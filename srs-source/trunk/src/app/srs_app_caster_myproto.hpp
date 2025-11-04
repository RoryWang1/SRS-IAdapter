//
// Copyright (c) 2013-2025 The SRS Authors
//
// SPDX-License-Identifier: MIT
//

#ifndef SRS_APP_CASTER_MYPROTO_HPP
#define SRS_APP_CASTER_MYPROTO_HPP

#include <srs_core.hpp>
#include <string>

class SrsConfDirective;
class SrsServer;
class ISrsListener;

ISrsListener* srs_create_myproto_caster_listener(SrsServer* srs, SrsConfDirective* conf);

#endif
