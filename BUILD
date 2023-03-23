filegroup(
  name = "all_headers",
  srcs = glob([
    "*.h"
  ]),
  visibility = [
    "//visibility:public"
  ],
)

cc_library(
  name = "dumpling_lib",
  hdrs = [
    ":all_headers"
  ],
  visibility = [
    "//visibility:public",
  ],
  alwayslink = true
)

