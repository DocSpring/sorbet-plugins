# typed: true
class DynamicUploader < Shrine
  def self.plugin_name = :derivatives

  plugin plugin_name # error: can't read this `plugin`: the plugin reads a Shrine plugin given as a symbol or a constant
end
