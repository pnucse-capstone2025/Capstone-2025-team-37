#ifndef TA_WAMR_H
#define TA_WAMR_H

#define TA_WAMR_UUID \
  { 0xb4c5d6e7, 0xf8a9, 0x4321, \
    { 0x87, 0x65, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc } }

#define COMMAND_RUN_WASM        0
#define COMMAND_CONFIGURE_HEAP  1
// Future: resume WASM execution after host handled a proxy request (GET/PUT)
#define COMMAND_RESUME_WASM     2

#endif /* TA_WAMR_H */
