# typed: true
# Plugins whose attachment methods the plugin can't type.
class OddUploader < Shrine
  plugin NamedPlugin # error: can't read this `plugin`: doesn't type the attachment methods that `NamedPlugin` defines
end
