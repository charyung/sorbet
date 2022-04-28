# frozen_string_literal: true
# typed: strict

class Project::Foo::Foo
  puts Project::Bar::This
     # ^^^^^^^^^^^^^^^^^^ error: Used constant `Project::Bar::This` from non-imported package `Project::Bar::This`
end
