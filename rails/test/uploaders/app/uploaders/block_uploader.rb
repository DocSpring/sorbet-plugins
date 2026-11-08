# typed: true
class BlockUploader < Shrine
  %i[derivatives].each { |name| plugin name } # error: can't read `plugin` here: the plugin reads it directly in an uploader's class body, or in an `if` or `unless` there
end
