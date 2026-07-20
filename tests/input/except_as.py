def bare():
    try:
        a = 1
    except:
        a = 2
    b = 3


def typed():
    try:
        a = 1
    except ValueError:
        a = 2
    b = 3


def alias():
    try:
        a = 1
    except ValueError as e:
        a = e
    b = 3
