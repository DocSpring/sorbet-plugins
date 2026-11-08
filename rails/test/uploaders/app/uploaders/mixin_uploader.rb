# typed: true
class MixinUploader < Shrine
  plugin MixinPlugin # error: can't read this `plugin`: doesn't type the modules that `MixinPlugin.configure` mixes in
end
