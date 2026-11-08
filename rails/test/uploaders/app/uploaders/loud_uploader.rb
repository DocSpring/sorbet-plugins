# typed: true
class LoudUploader < Shrine
  plugin LoudPlugin # error: can't read this `plugin`: can't read `LoudPlugin.load_dependencies` (line 4): the plugin reads `uploader.plugin` calls
end
