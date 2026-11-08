# typed: true
class SplatUploader < Shrine
  OPTIONS = { versions_compatibility: true }.freeze

  plugin :derivatives, **OPTIONS # error: can't read this `plugin`: can't read `versions_compatibility:` among the options: the plugin reads them written out
end
