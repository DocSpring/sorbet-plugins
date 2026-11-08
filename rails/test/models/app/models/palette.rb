# typed: true
# Constants that another file's enums use.
module Palette
  SHADES = { light: 'light', dark: 'dark' }.freeze
  TONES = T.let(%i[warm cool], T::Array[Symbol])
end
