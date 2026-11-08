# typed: true
class UnknownUploader < Shrine
  plugin :signature # error: can't read this `plugin`: doesn't know Shrine's `signature` plugin: add it to the table in the plugin's rails/Uploaders.cc
end
