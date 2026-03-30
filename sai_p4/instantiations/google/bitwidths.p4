#ifndef PINS_SAI_BITWIDTHS_P4_
#define PINS_SAI_BITWIDTHS_P4_

#ifdef PLATFORM_BMV2
  // Number of bits used for types that use @p4runtime_translation("", string).
  // This allows BMv2 to support string up to this length.
  #define STRING_MAX_BITWIDTH 256 // 32 chars

  // TODO: We want to use the commented definition, but BMv2 does not
  // support large numbers for ports.
  // #define PORT_BITWIDTH STRING_MAX_BITWIDTH
  #define PORT_BITWIDTH 9
  #define VRF_BITWIDTH STRING_MAX_BITWIDTH
  #define NEXTHOP_ID_BITWIDTH STRING_MAX_BITWIDTH
  #define ROUTER_INTERFACE_ID_BITWIDTH STRING_MAX_BITWIDTH
  #define WCMP_GROUP_ID_BITWIDTH STRING_MAX_BITWIDTH
  #define MIRROR_SESSION_ID_BITWIDTH STRING_MAX_BITWIDTH
  #define QOS_QUEUE_BITWIDTH STRING_MAX_BITWIDTH
  #define TUNNEL_ID_BITWIDTH STRING_MAX_BITWIDTH
#elif defined(PLATFORM_4WARD)
  // 4ward uses 16-bit ports (matching v1model_sai.p4's standard_metadata).
  // Other bitwidths use the defaults below.
  #define PORT_BITWIDTH 16
#endif

// Defaults for p4-symbolic, p4testgen, 4ward, etc.
#ifndef PORT_BITWIDTH
  #define PORT_BITWIDTH 9
#endif
#ifndef VRF_BITWIDTH
  #define VRF_BITWIDTH 10
#endif
#ifndef NEXTHOP_ID_BITWIDTH
  #define NEXTHOP_ID_BITWIDTH 10
#endif
#ifndef ROUTER_INTERFACE_ID_BITWIDTH
  #define ROUTER_INTERFACE_ID_BITWIDTH 10
#endif
#ifndef WCMP_GROUP_ID_BITWIDTH
  #define WCMP_GROUP_ID_BITWIDTH 12
#endif
#ifndef MIRROR_SESSION_ID_BITWIDTH
  #define MIRROR_SESSION_ID_BITWIDTH 10
#endif
#ifndef QOS_QUEUE_BITWIDTH
  #define QOS_QUEUE_BITWIDTH 8
#endif
#ifndef TUNNEL_ID_BITWIDTH
  #define TUNNEL_ID_BITWIDTH 10
#endif

#endif  // PINS_SAI_BITWIDTHS_P4_
