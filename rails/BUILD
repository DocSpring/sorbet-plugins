# Each template in templates/ becomes a string constant, `templates::<name>` (Template.h).
genrule(
    name = "templates",
    srcs = glob(["templates/*.rbi"]),
    outs = ["Templates.cc"],
    cmd = """
{
  echo '#include <string_view>'
  echo 'namespace sorbet::plugins::rails::templates {'
  for f in $(SRCS); do
    name=$$(basename $$f .rbi)
    echo "extern const std::string_view $$name;"
    printf 'const std::string_view %s = R"RBI(' "$$name"
    cat $$f
    echo ')RBI";'
  done
  echo '}'
} > $@
""",
)

cc_library(
    name = "plugin",
    srcs = glob(["*.cc", "*.h"]) + [":templates"],
    alwayslink = True,
    visibility = ["//visibility:public"],
    deps = [
        "@//plugins:api",
        "@//rewriter/util",
        "@prism",
    ],
)
