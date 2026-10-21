# typed: true

# Stand-ins for what generate_rbi.rb writes from the app's RSpec configuration.
module RSpecConfig
  module Everywhere
    def expect(value = nil, &blk); end
    def eq(value); end
  end

  module Type
    module Request
      def get(path); end
    end

    module Model
      def model_helper; end
    end
  end

  module Extend
    module Everywhere
      def test_each(iterable, &blk); end
      def test_each_hash(iterable, &blk); end
    end

    module Type
      module Request; end
      module Model; end
    end
  end
end

module RSpec
  module Core
    class ExampleGroup; end

    class Example
      def metadata; end

      class Procsy
        def run; end
      end
    end
  end
end

class User
  def self.all; end
end
