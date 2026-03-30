// Shim that shadows stock <v1model.p4> for 4ward builds.
// The fourward_pipeline rule adds -I sai_p4/fixed/fourward, so
// #include <v1model.p4> resolves here instead of to p4include/v1model.p4.
#include "../v1model_sai.p4"
