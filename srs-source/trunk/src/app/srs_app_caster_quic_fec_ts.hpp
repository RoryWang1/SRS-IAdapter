//
// Copyright (c) 2013-2025 The SRS Authors
//
// SPDX-License-Identifier: MIT
//

#ifndef SRS_APP_CASTER_QUIC_FEC_TS_HPP
#define SRS_APP_CASTER_QUIC_FEC_TS_HPP

#include <srs_app_listener.hpp>

class SrsServer;
class SrsConfDirective;

ISrsListener* srs_create_quic_fec_ts_caster_listener(SrsServer* srs, SrsConfDirective* conf);

#endif

