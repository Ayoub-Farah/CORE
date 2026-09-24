#pragma once
#include <zcbor_common.h>
struct cbor_nb_reader { zcbor_state_t zs[6]; };
struct cbor_nb_writer { zcbor_state_t zs[6]; };
struct smp_streamer { cbor_nb_reader *reader; cbor_nb_writer *writer; };
