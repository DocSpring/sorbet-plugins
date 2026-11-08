# typed: true
module Shop
  class Order < ApplicationRecord # error: `Shop::Order` needs `self.table_name = '...'`
  end
end
