# typed: true
class OptionUploader < Shrine
  plugin :derivatives, versions_compatibility: ENV.key?('VERSIONS') # error: can't read this `plugin`: can't read `versions_compatibility:`: the plugin reads `true` or `false`
end
