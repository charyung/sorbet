# typed: true

class Foo
  def bar
    my_local_variable = 123
#   ^ apply-rename: [A] newName: my_new_local_variable
    process(my_local_variable)
    my_local_variable
  end

  def baz
    my_local_variable = 123
    my_local_variable.to_s
  end

  private

  def process(a)
  end
end

class Bar
  def foo
    a = "hello"
#   ^ apply-rename: [E] newName: a invalid: true expectedErrorMessage: The new name cannot be the same as the oldname.
  end
end

class DifferentScopeLevels
  def foo
    variable = "123"
#   ^ apply-rename: [B] newName: new_variable

    other = [1, 2, 3].map do |variable|
      variable.to_s
    end

    variable
  end

  def bar
    5.times do
      local = 123
    # ^ apply-rename: [C] newName: new_local
      local * 5
    end

    local = "something different"
  # ^ apply-rename: [G] newName: new_local
    local
  end

  def cond
    if Random.rand(2).even?
      x = 0
    # ^ apply-rename: [D] newName: y
    else
      x = ""
    end
  end

  def higher_level
    higher = 123
  # ^ apply-rename: [F] newName: new_higher

    5.times do
      higher = 321
    end

    higher
  end
end

class MethodParameter
  def add(a, b)
        # ^ apply-rename: [H] newName: c
    a + b
  end
end

# TODO: change the expectations to new_variable in the block parameter
# definition once the bug with find all references is fixed
class BlockParameter
  def foo
    variable = 1

    [1, 2, 3].map do |variable| 
                    # ^ apply-rename: [I] newName: new_variable
      variable.to_s
    end

    variable
  end

  def bar
    variable = 1

    [1, 2, 3].map do |variable| 
      variable.to_s
    # ^ apply-rename: [J] newName: new_variable
    end

    variable
  end
end
