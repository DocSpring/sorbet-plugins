# typed: true
class BareController < ApplicationController
  clear_helpers # error: rails plugin: `clear_helpers` isn't supported: the plugin would keep the helpers it removes
end
