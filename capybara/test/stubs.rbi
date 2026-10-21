# typed: true

module RSpec
  module Core
    class ExampleGroup
      def visit(path); end
      def click_on(text); end
      def expect(value = nil, &blk); end
      def have_content(text); end
    end
  end
end

class User
  def name; end
end
