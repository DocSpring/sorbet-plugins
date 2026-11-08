cc_library(
    name = "plugin",
    srcs = glob(["*.cc", "*.h"]),
    alwayslink = True,
    visibility = ["//visibility:public"],
    deps = [
        "@//plugins:api",
        "@//rewriter/util",
        "@prism",
    ],
)
