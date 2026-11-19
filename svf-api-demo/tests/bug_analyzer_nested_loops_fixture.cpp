int loop_sink = 0;

void nested_for_loops(int limit) {
    for (int i = 0; i < limit; ++i) {
        for (int j = 0; j < limit; ++j)
            loop_sink += i + j;
    }
}

void nested_while_loops(int limit) {
    int i = 0;
    while (i < limit) {
        int j = 0;
        while (j < limit) {
            loop_sink += i + j;
            ++j;
        }
        ++i;
    }
}

void nested_do_while_loops(int limit) {
    int i = 0;
    do {
        int j = 0;
        do {
            loop_sink += i + j;
            ++j;
        } while (j < limit);
        ++i;
    } while (i < limit);
}

void mixed_nested_loops(int limit) {
    for (int i = 0; i < limit; ++i) {
        int j = 0;
        while (j < limit) {
            int k = 0;
            do {
                loop_sink += i + j + k;
                ++k;
            } while (k < limit);
            ++j;
        }
    }
}

void four_nested_loops(int limit) {
    for (int i = 0; i < limit; ++i) {
        for (int j = 0; j < limit; ++j) {
            for (int k = 0; k < limit; ++k) {
                for (int m = 0; m < limit; ++m)
                    loop_sink += i + j + k + m;
            }
        }
    }
}

void nested_loops_in_branches(int limit, bool choose_first) {
    for (int i = 0; i < limit; ++i) {
        if (choose_first) {
            for (int j = 0; j < limit; ++j)
                loop_sink += i + j;
        } else {
            int j = 0;
            while (j < limit) {
                loop_sink += i + j;
                ++j;
            }
        }
    }
}

void sequential_nested_loop_pairs(int limit) {
    for (int i = 0; i < limit; ++i) {
        for (int j = 0; j < limit; ++j)
            loop_sink += i + j;
    }

    int k = 0;
    while (k < limit) {
        int m = 0;
        while (m < limit) {
            loop_sink += k + m;
            ++m;
        }
        ++k;
    }
}

void nested_range_based_loops() {
    const int outer_values[] = {1, 2, 3};
    const int inner_values[] = {4, 5};
    for (int outer : outer_values) {
        for (int inner : inner_values)
            loop_sink += outer + inner;
    }
}

void small_fixed_trip_nested_loops() {
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 3; ++j)
            loop_sink += i + j;
    }
}

void large_fixed_trip_nested_loops() {
    for (int i = 0; i < 20; ++i) {
        for (int j = 0; j < 20; ++j)
            loop_sink += i + j;
    }
}

void nested_loop_side_effect(int i, int j) {
    loop_sink += i + j;
}

void small_fixed_trip_nested_loops_with_call() {
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 3; ++j)
            nested_loop_side_effect(i, j);
    }
}

void nested_loops_in_lambda(int count) {
    const auto process_batch = [&]() {
        for (int row = 0; row < count; ++row) {
            for (int column = 0; column < count; ++column)
                loop_sink += row * count + column;
        }
    };
    process_batch();
}

void sequential_loops(int limit) {
    for (int i = 0; i < limit; ++i)
        loop_sink += i;

    int j = 0;
    while (j < limit) {
        loop_sink += j;
        ++j;
    }

    int k = 0;
    do {
        loop_sink += k;
        ++k;
    } while (k < limit);
}

void loops_in_separate_branches(int limit, bool choose_first) {
    if (choose_first) {
        for (int i = 0; i < limit; ++i)
            loop_sink += i;
    } else {
        int j = 0;
        while (j < limit) {
            loop_sink += j;
            ++j;
        }
    }
}

int main() {
    nested_for_loops(2);
    nested_while_loops(2);
    nested_do_while_loops(2);
    mixed_nested_loops(2);
    four_nested_loops(2);
    nested_loops_in_branches(2, true);
    sequential_nested_loop_pairs(2);
    nested_range_based_loops();
    small_fixed_trip_nested_loops();
    large_fixed_trip_nested_loops();
    small_fixed_trip_nested_loops_with_call();
    nested_loops_in_lambda(2);
    sequential_loops(2);
    loops_in_separate_branches(2, false);
    return loop_sink == -1;
}
