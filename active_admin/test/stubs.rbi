# typed: true

class User
  def name; end
  def email; end
  def account; end
end

module ActiveAdmin
  def self.register(resource, options = {}, &blk); end
  def self.register_page(name, options = {}, &blk); end

  class DSL
    def menu(options = {}); end
    def controller(&blk); end
    def action_item(name, options = {}, &blk); end
    def sidebar(name, options = {}, &blk); end
  end

  class ResourceDSL < DSL
    def filter(attribute, options = {}); end
    def permit_params(*args, &blk); end
    def index(options = {}, &blk); end
    def show(options = {}, &blk); end
    def form(options = {}, &blk); end
    def csv(options = {}, &blk); end
    def member_action(name, options = {}, &blk); end
    def collection_action(name, options = {}, &blk); end
    def batch_action(name, options = {}, &blk); end
    def before_create(&blk); end
  end

  class PageDSL < DSL
    def content(options = {}, &blk); end
    def page_action(name, options = {}, &blk); end
  end

  class FormBuilder
    def inputs(*args, &blk); end
    def input(name, options = {}); end
    def actions; end
  end

  class BaseController
    def params; end
    def redirect_to(*args); end
    def current_user; end
    def self.before_action(*names, &blk); end
  end

  class ResourceController < BaseController
    def resource; end
  end

  class PageController < BaseController; end
end

module Admin
  class UsersController < ActiveAdmin::ResourceController; end
  class DashboardController < ActiveAdmin::PageController; end
end

# What generate_rbi.rb writes for `ActiveAdmin.register User` and `register_page 'Dashboard'`.
module ActiveAdminTypes
  class View
    def column(*args, **kwargs, &blk); end
    def row(*args, **kwargs, &blk); end
    def attributes_table(*args, **kwargs, &blk); end
    def table_for(*args, **kwargs, &blk); end
    def panel(*args, **kwargs, &blk); end
    def actions(*args, **kwargs, &blk); end
    def link_to(*args, **kwargs, &blk); end
    def para(*args, **kwargs, &blk); end
    def current_user; end
  end

  module Resources
    module User
      class Controller < ::Admin::UsersController
        sig { returns(::User) }
        def resource; end
      end

      class View < ::ActiveAdminTypes::View
        sig { returns(::User) }
        def resource; end

        sig { returns(::User) }
        def user; end
      end
    end
  end

  module Pages
    module Dashboard
      class Controller < ::Admin::DashboardController; end
      class View < ::ActiveAdminTypes::View; end
    end
  end
end
