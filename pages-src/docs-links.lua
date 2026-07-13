function Link(element)
    local target = element.target

    if target:match("^[a-zA-Z][a-zA-Z0-9+.-]*:") then
        return element
    end

    local path, anchor = target:match("^(.-)%.md(#.*)$")
    if path then
        element.target = path .. ".html" .. anchor
        return element
    end

    path = target:match("^(.-)%.md$")
    if path then
        element.target = path .. ".html"
    end

    return element
end
