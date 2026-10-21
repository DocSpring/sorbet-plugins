# typed: true

module RSpec
  module Core
    class ExampleGroup
      def response; end
      def expect(value = nil, &blk); end
      def eq(value); end
      def self.parameter(**options); end
      def self.produces(*types); end
      def self.run_test!(description = nil, *args, &blk); end
    end
  end
end
