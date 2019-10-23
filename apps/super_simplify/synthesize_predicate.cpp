#include "synthesize_predicate.h"
#include "expr_util.h"
#include "super_simplify.h"
#include "z3.h"
#include "Halide.h"

using namespace Halide;
using namespace Halide::Internal;

using std::vector;
using std::map;
using std::set;
using std::string;
using std::pair;

template<typename Op>
vector<Expr> unpack_binary_op(const Expr &e) {
    vector<Expr> pieces, pending;
    pending.push_back(e);
    while (!pending.empty()) {
        Expr next = pending.back();
        pending.pop_back();
        if (const Op *op = next.as<Op>()) {
            pending.push_back(op->a);
            pending.push_back(op->b);
        } else {
            pieces.push_back(next);
        }
    }
    return pieces;
}

template<typename Op, typename Iterable>
Expr pack_binary_op(const Iterable &v) {
    Expr result;
    for (const Expr &e : v) {
        if (result.defined()) {
            result = Op::make(result, e);
        } else {
            result = e;
        }
    }
    return result;
}

// Make an auxiliary variable
Expr aux() {
    Var k(unique_name('k'));
    return k;
}

// A mostly-linear constraint. Represented as a linear combination
// of terms that sum to zero. The terms are usually Variables, but
// may be non-linear functions of Variables too.
struct Equality {

    // We keep the terms unique by storing them in a map sorted by
    // deep equality on the Exprs.
    std::map<Expr, int, IRDeepCompare> terms;

    // Track the number of terms that are just Variable
    // nodes. Useful for prioritizing work.
    int num_vars = 0;

    // Recursively extract all the linear terms from an Expr
    void find_terms(const Expr &e, int c) {
        if (c == 0) return;
        if (is_zero(e)) return;
        const Add *add = e.as<Add>();
        const Sub *sub = e.as<Sub>();
        const Mul *mul = e.as<Mul>();
        const int64_t *coeff = (mul ? as_const_int(mul->b) : nullptr);
        if (coeff && mul_would_overflow(64, c, *coeff)) {
            coeff = nullptr;
        }
        if (add) {
            find_terms(add->a, c);
            find_terms(add->b, c);
        } else if (sub) {
            find_terms(sub->a, c);
            find_terms(sub->b, -c);
        } else if (mul && coeff) {
            find_terms(mul->a, c * (int)(*coeff));
        } else if (mul && is_const(mul->a)) {
            find_terms(mul->b * mul->a, c);
        } else if (mul) {
            // Apply distributive law to non-linear terms
            const Add *a_a = mul->a.as<Add>();
            const Sub *s_a = mul->a.as<Sub>();
            const Add *a_b = mul->b.as<Add>();
            const Sub *s_b = mul->b.as<Sub>();
            const Variable *v_a = mul->a.as<Variable>();
            const Variable *v_b = mul->b.as<Variable>();
            if (a_a) {
                find_terms(a_a->a * mul->b, c);
                find_terms(a_a->b * mul->b, c);
            } else if (s_a) {
                find_terms(s_a->a * mul->b, c);
                find_terms(s_a->b * mul->b, -c);
            } else if (a_b) {
                find_terms(mul->a * a_b->a, c);
                find_terms(mul->a * a_b->b, c);
            } else if (s_b) {
                find_terms(mul->a * s_b->a, c);
                find_terms(mul->a * s_b->b, -c);
            } else if (v_a && v_b) {
                // Canonicalize products
                if (v_a->name < v_b->name) {
                    add_term(e, c);
                } else {
                    add_term(mul->b * mul->a, c);
                }
            } else {
                add_term(e, c);
            }
        } else {
            add_term(e, c);
        }
    }

    void add_term(const Expr &e, int c) {
        auto p = terms.emplace(e, c);
        if (!p.second) {
            p.first->second += c;
            if (p.first->second == 0) {
                terms.erase(p.first);
                if (e.as<Variable>()) {
                    num_vars--;
                }
            }
        } else if (e.as<Variable>()) {
            num_vars++;
        }
    }


    Equality(const EQ *eq) {
        find_terms(eq->a, 1);
        find_terms(eq->b, -1);
    }
    Equality() = default;

    bool uses_var(const std::string &name) const {
        for (const auto &p : terms) {
            if (expr_uses_var(p.second, name)) {
                return true;
            }
        }
        return false;
    }

    // Convert this constraint back to a boolean Expr by putting
    // all the positive coefficients on one side and all the
    // negative coefficients on the other.
    Expr to_expr() const {
        Expr lhs, rhs;
        auto accum = [](Expr &a, Expr e, int c) {
            Expr t = e;
            if (c != 1) {
                t *= c;
            }
            if (a.defined()) {
                a += t;
            } else {
                a = t;
            }
        };
        for (auto p : terms) {
            if (p.second > 0) {
                accum(lhs, p.first, p.second);
            } else {
                accum(rhs, p.first, -p.second);
            }
        }
        if (!lhs.defined()) {
            lhs = 0;
        }
        if (!rhs.defined()) {
            rhs = 0;
        }
        return lhs == rhs;
    }
};

// A system of constraints. We're going to construct systems of
// constraints that have solutions that are all of the correctness
// violations (places where one Func clobbers a value in the
// shared buffer that the other Func still needs), and then try to
// prove that these systems have no solutions by finding a
// sequence of variable substitutions that turns one of the terms
// into the constant false.
struct System {

    // A bunch of equalities
    vector<Equality> equalities;

    // A shared simplifier instance, which slowly accumulates
    // knowledge about the bounds of different variables. We
    // prefer to encode constraints this way whenever we can,
    // because then they get automatically exploited whenever we
    // simplify something. Ultimately the way we prove things
    // infeasible is by slowly deducing bounds on the free
    // variables and then finding that one of our equalities above
    // can't be satisfied given the bounds we have learned.
    Simplify *simplifier;

    // The most-recently-performed substition, for debugging
    Expr most_recent_substitution;

    // An additional arbitrary term to place non-linear constraints
    Expr non_linear_term;

    // A heuristic for how close we are to finding infeasibility
    float c = 0;

    // unique IDs for each system for debugging and training a good heuristic
    static uint64_t id_counter;
    uint64_t id, parent_id;

    System(Simplify *s, Expr subs, int pid) :
        simplifier(s), most_recent_substitution(subs),
        id(id_counter++), parent_id(pid) {}

    void add_equality(const EQ *eq) {
        equalities.emplace_back(eq);
    }

    void add_non_linear_term(const Expr &e) {
        _halide_user_assert(e.type().is_bool()) << e << "\n";
        if (is_zero(e) || !non_linear_term.defined()) {
            non_linear_term = e;
        } else {
            non_linear_term = non_linear_term && e;
        }
    }

    bool can_prove(Expr e) {
        return is_one(simplifier->mutate(e, nullptr));
    }

    void add_term(const Expr &e) {
        const EQ *eq = e.as<EQ>();
        const LT *lt = e.as<LT>();
        const LE *le = e.as<LE>();
        if (eq && eq->a.type() == Int(32)) {
            add_equality(eq);
        } else if (const And *a = e.as<And>()) {
            add_term(a->a);
            add_term(a->b);
        } else if (const GT *gt = e.as<GT>()) {
            add_term(gt->b < gt->a);
        } else if (const GE *ge = e.as<GE>()) {
            add_term(ge->b <= ge->a);
        } else if (le && le->a.type() == Int(32)) {
            const Variable *va = le->a.as<Variable>();
            const Variable *vb = le->b.as<Variable>();
            if (const Min *min_b = le->b.as<Min>()) {
                // x <= min(y, z) -> x <= y && x <= z
                add_term(le->a <= min_b->a);
                add_term(le->a <= min_b->b);
            } else if (const Max *max_a = le->a.as<Max>()) {
                // max(x, y) <= z -> x <= z && y <= z
                add_term(max_a->a <= le->b);
                add_term(max_a->b <= le->b);
            } else if (is_const(le->a) && vb) {
                simplifier->learn_true(e);
            } else if (is_const(le->b) && va) {
                simplifier->learn_true(e);
            } else {
                Expr v = aux();
                simplifier->learn_true(0 <= v);
                add_term(le->a + v == le->b);
                simplifier->learn_true(e);
            }
        } else if (lt && lt->a.type() == Int(32)) {
            const Variable *va = lt->a.as<Variable>();
            const Variable *vb = lt->b.as<Variable>();
            if (const Min *min_b = lt->b.as<Min>()) {
                // x <= min(y, z) -> x <= y && x <= z
                add_term(lt->a < min_b->a);
                add_term(lt->a < min_b->b);
            } else if (const Max *max_a = lt->a.as<Max>()) {
                // max(x, y) <= z -> x <= z && y <= z
                add_term(max_a->a < lt->b);
                add_term(max_a->b < lt->b);
            } else if (is_const(lt->a) && vb) {
                simplifier->learn_true(e);
            } else if (is_const(lt->b) && va) {
                simplifier->learn_true(e);
            } else {
                Expr v = aux();
                simplifier->learn_true(0 <= v);
                add_term(lt->a + v + 1 == lt->b);
                simplifier->learn_true(e);
            }
        } else if (const Let *l = e.as<Let>()) {
            // Treat lets as equality constraints in the new variable.
            if (l->value.type().is_bool()) {
                // We want to examine booleans more directly, so
                // substitute them in.
                add_term(substitute(l->name, l->value, l->body));
            } else {
                Expr eq = Variable::make(l->value.type(), l->name) == l->value;
                simplifier->learn_true(eq);
                add_term(eq);
                add_term(l->body);
            }
        } else if (is_one(e)) {
            // There's nothing we can learn from a tautology
        } else {
            // If all else fails, treat it as a non-linearity
            add_non_linear_term(e);
        }
    }

    void dump() const {
        if (most_recent_substitution.defined()) {
            debug(1) << "Substitution: " << most_recent_substitution << "\n";
        }
        for (auto &e : equalities) {
            debug(1) << " " << e.to_expr() << "\n";
        }
        if (non_linear_term.defined()) {
            debug(1) << " non-linear: " << non_linear_term << "\n";
        }
        const auto &info = simplifier->bounds_and_alignment_info;
        for (auto it = info.cbegin(); it != info.cend(); ++it){
            bool used = false;
            for (auto &e : equalities) {
                used |= expr_uses_var(e.to_expr(), it.name());
            }
            if (non_linear_term.defined()) {
                used |= expr_uses_var(non_linear_term, it.name());
            }
            if (!used) continue;
            if (it.value().min_defined && it.value().max_defined) {
                debug(1) << " " << it.value().min << " <= " << it.name()
                         << " <= " << it.value().max << "\n";
            } else if (it.value().min_defined) {
                debug(1) << " " << it.value().min << " <= " << it.name() << "\n";
            } else if (it.value().max_defined) {
                debug(1) << " " << it.name() << " <= " << it.value().max << "\n";
            }
        }
    }

    bool infeasible() const {
        // Check if any of the equalities or the non-linear term
        // are unsatisfiable or otherwise simplify to const false
        // given all the knowledge we have accumulated into the
        // simplifier instance.
        for (auto &e : equalities) {
            if (is_zero(simplifier->mutate(e.to_expr(), nullptr))) {
                return true;
            }
        }
        if (non_linear_term.defined() && is_zero(simplifier->mutate(non_linear_term, nullptr))) {
            return true;
        }
        return false;
    }

    void finalize() {
        // We'll preferentially find substitutions from the
        // earlier equations, so sort the system, putting low
        // term-count expressions with lots of naked vars first
        std::stable_sort(equalities.begin(), equalities.end(),
                         [](const Equality &a, const Equality &b) {
                             if (a.terms.size() < b.terms.size()) return true;
                             if (a.terms.size() > b.terms.size()) return false;
                             return a.num_vars < b.num_vars;
                         });
        compute_complexity();
    }

    // Compute our heuristic for which systems are closest to infeasible
    void compute_complexity() {
        class HasNonConstantVar : public IRVisitor {
            void visit(const Variable *op) {
                result |= (op->name[0] != 'c');
            }
        public:
            bool result = false;
        };

        std::map<std::string, int> inequalities;
        int non_linear_terms = 0, num_terms = 0;
        std::set<std::string> wild_constant_terms;
        int useful_implications = 0;
        for (auto &e : equalities) {
            bool lhs_has_symbolic_lower_bound = true;
            bool rhs_has_symbolic_lower_bound = true;
            bool lhs_has_symbolic_upper_bound = true;
            bool rhs_has_symbolic_upper_bound = true;
            for (auto &p : e.terms) {
                HasNonConstantVar h;
                p.first.accept(&h);
                Simplify::ExprInfo info;
                simplifier->mutate(p.first, &info);
                bool has_symbolic_lower_bound = !h.result || info.min_defined;
                bool has_symbolic_upper_bound = !h.result || info.max_defined;
                if (p.second > 0) {
                    rhs_has_symbolic_lower_bound &= has_symbolic_lower_bound;
                    rhs_has_symbolic_upper_bound &= has_symbolic_upper_bound;
                } else {
                    lhs_has_symbolic_lower_bound &= has_symbolic_lower_bound;
                    lhs_has_symbolic_upper_bound &= has_symbolic_upper_bound;
                }
                if (const Variable *var = p.first.as<Variable>()) {
                    inequalities[var->name] = (int)info.max_defined + (int)info.min_defined;
                    if (var->name[0] == 'c') {
                        wild_constant_terms.insert(var->name);
                    }
                } else if (!is_const(p.first)) {
                    non_linear_terms++;
                }
                num_terms++;
            }
            if (lhs_has_symbolic_lower_bound && rhs_has_symbolic_upper_bound) {
                useful_implications++;
            }
            if (rhs_has_symbolic_lower_bound && lhs_has_symbolic_upper_bound) {
                useful_implications++;
            }
        }

        int unconstrained_vars = 0;
        int semi_constrained_vars = 0;
        int totally_constrained_vars = 0;
        int num_constraints = (int)equalities.size() + (int)non_linear_term.defined();
        for (const auto &p : inequalities) {
            if (p.second == 0) {
                unconstrained_vars++;
            } else if (p.second == 1) {
                semi_constrained_vars++;
            } else {
                totally_constrained_vars++;
            }
        }
        debug(1) << "FEATURES " << id
                 << " " << parent_id
                 << " " << non_linear_terms
                 << " " << unconstrained_vars
                 << " " << semi_constrained_vars
                 << " " << totally_constrained_vars
                 << " " << num_terms
                 << " " << num_constraints
                 << " " << useful_implications
                 << " " << wild_constant_terms.size() << "\n";
        int terms[] = {non_linear_terms, unconstrained_vars, semi_constrained_vars,
                       totally_constrained_vars, num_terms, num_constraints,
                       useful_implications, (int)wild_constant_terms.size()};
        // Use a linear combination of these features to decide
        // which stats are the most promising to explore. Trained
        // by tracking which states lead to success in the
        // store_with test and minimizing cross-entropy loss on a
        // linear classifier.
        float coeffs[] = {-0.1575f, -0.2994f,  0.0849f,  0.1400f,
                          -0.0252f,  0.4637f,  1.0000f,  0.3821f};

        c = 0.0f;
        for (int i = 0; i < 8; i++) {
            c -= terms[i] * coeffs[i];
        }
    }

    float complexity() const {
        return c;
    }

    Expr exact_divide(const Expr &e, const std::string &v) {
        if (const Variable *var = e.as<Variable>()) {
            if (var->name == v) {
                return make_one(e.type());
            } else {
                return Expr();
            }
        } else if (const Mul *mul = e.as<Mul>()) {
            Expr a = exact_divide(mul->a, v);
            if (a.defined()) {
                return a * mul->b;
            }
            Expr b = exact_divide(mul->b, v);
            if (b.defined()) {
                return mul->a * b;
            }
        }
        return Expr();
    }

    void make_children(std::deque<std::unique_ptr<System>> &result) {

        size_t old_size = result.size();

        // Eliminate divs and mods by introducing new variables
        for (int i = 0; i < (int)equalities.size(); i++) {
            Expr lhs, rhs;
            for (auto &p : equalities[i].terms) {
                const Mod *mod = p.first.as<Mod>();
                const Div *div = p.first.as<Div>();
                const Mul *mul = p.first.as<Mul>();
                if (mod) {
                    lhs = mod->a;
                    rhs = mod->b;
                } else if (div) {
                    lhs = div->a;
                    rhs = div->b;
                } else if (mul) {
                    lhs = mul->a;
                    rhs = mul->b;
                }

                if (is_const(rhs)) {
                    _halide_user_assert(mul == nullptr);
                    break;
                } else if (const Variable *v = rhs.as<Variable>()) {
                    // HACK for constant vars
                    if (mul) {
                        div = mul->a.as<Div>();
                        if (div) {
                            lhs = div->a;
                        }
                    }
                    if (starts_with(v->name, "c") &&
                        (!mul || (div && equal(div->b, mul->b))) &&
                        is_one(simplifier->mutate(rhs > 0, nullptr))) {
                        break;
                    }
                }

                lhs = rhs = Expr();
            }
            if (lhs.defined()) {
                Expr k1 = aux(), k2 = aux();
                Expr replacement = simplifier->mutate(k1 + k2 * rhs, nullptr);
                auto subs = [&](Expr e) {
                    e = substitute(lhs % rhs, k1, e);
                    e = substitute(lhs / rhs, k2, e);
                    return simplifier->mutate(e, nullptr);
                };
                std::unique_ptr<System> new_system(new System(simplifier, lhs == rhs, id));
                if (non_linear_term.defined()) {
                    new_system->add_term(subs(non_linear_term));
                }
                for (int j = 0; j < (int)equalities.size(); j++) {
                    new_system->add_term(subs(equalities[j].to_expr()));
                }
                new_system->add_term(lhs == replacement);
                simplifier->learn_true(0 <= k1);
                if (is_const(rhs)) {
                    simplifier->learn_true(k1 < rhs);
                } else {
                    // TODO: only if we know RHS is positive.
                    new_system->add_term(k1 < rhs);
                }
                new_system->finalize();
                result.emplace_back(std::move(new_system));
                return;
            }
        }

        // Divide through by common factors
        for (int i = 0; i < (int)equalities.size(); i++) {
            std::map<std::string, int> factors;
            for (auto &p : equalities[i].terms) {
                for (const Expr &next : unpack_binary_op<Mul>(p.first)) {
                    if (const Variable *v = next.as<Variable>()) {
                        factors[v->name]++;
                    }
                }
            }
            for (const auto &f : factors) {
                if (f.second == 1) {
                    // Not a repeated factor
                    continue;
                }
                Expr factor_expr = Variable::make(Int(32), f.first);
                Expr terms_with_factor = 0;
                Expr terms_without_factor = 0;
                for (auto &p : equalities[i].terms) {
                    Expr e = exact_divide(p.first, f.first);
                    if (e.defined()) {
                        terms_with_factor += e * p.second;
                    } else {
                        terms_without_factor += p.first * p.second;
                    }
                }
                terms_with_factor = simplifier->mutate(terms_with_factor, nullptr);

                if (is_zero(simplifier->mutate(terms_without_factor == 0, nullptr))) {
                    // If the sum of the terms that do not reference
                    // the factor can't be zero, then the factor can't
                    // be zero either, so it's safe to divide
                    // by. Furthermore, this implies that the terms
                    // with the factor can't sum to zero.
                    std::unique_ptr<System> new_system(new System(simplifier, Expr(), id));
                    if (non_linear_term.defined()) {
                        new_system->add_term(non_linear_term);
                    }
                    for (int j = 0; j < (int)equalities.size(); j++) {
                        if (i != j) {
                            new_system->add_term(equalities[j].to_expr());
                        }
                    }
                    new_system->add_term(terms_with_factor != 0);
                    new_system->finalize();
                    result.emplace_back(std::move(new_system));
                }
            }
        }

        // Replace repeated non-linear terms with new variables
        std::map<Expr, int, IRDeepCompare> nonlinear_terms;
        for (int i = 0; i < (int)equalities.size(); i++) {
            for (auto &p : equalities[i].terms) {
                if (!p.first.as<Variable>() && !is_const(p.first)) {
                    nonlinear_terms[p.first]++;
                }
            }
        }

        for (auto p : nonlinear_terms) {
            if (p.second > 1) {
                // It's a repeated non-linearity. Replace it with an
                // opaque variable so that we can try cancelling it.
                Var t(unique_name('n'));

                debug(1) << "Repeated non-linear term: " << t << " == " << p.first << "\n";

                auto subs = [&](Expr e) {
                    e = substitute(p.first, t, e);
                    return e;
                };

                std::unique_ptr<System> new_system(new System(simplifier, t == p.first, id));
                if (non_linear_term.defined()) {
                    new_system->add_term(subs(non_linear_term));
                }
                for (int j = 0; j < (int)equalities.size(); j++) {
                    new_system->add_term(subs(equalities[j].to_expr()));
                }

                // Carry over any bounds on the non-linear term to a bound on the new variable.
                Simplify::ExprInfo bounds;
                simplifier->mutate(p.first, &bounds);
                if (bounds.min_defined) {
                    simplifier->learn_true(t >= (int)bounds.min);
                }
                if (bounds.max_defined) {
                    simplifier->learn_true(t <= (int)bounds.max);
                }

                new_system->finalize();
                result.emplace_back(std::move(new_system));
            }
        }

        // Which equations should we mine for
        // substitutions. Initially all of them are promising.
        vector<bool> interesting(equalities.size(), true);

        // A list of all variables we could potentially eliminate
        set<string> eliminable_vars;
        for (int i = 0; i < (int)equalities.size(); i++) {
            for (const auto &p : equalities[i].terms) {
                const Variable *var = p.first.as<Variable>();
                // HACK: forbid use of constant wildcards.
                // if (var && starts_with(var->name, "c")) continue;
                if (var && (p.second == 1 || p.second == -1)) {
                    eliminable_vars.insert(var->name);
                }
            }
        }

        if (!equalities.empty() && eliminable_vars.empty()) {
            debug(1) << "NO ELIMINABLE VARS:\n";
            dump();
        }

        /*
          for (const auto &v : eliminable_vars) {
          debug(1) << "Eliminable var: " << v << "\n";
          }
        */

        // A mapping from eliminable variables to the equalities that reference them.
        map<string, vector<int>> eqs_that_reference_var;
        for (int i = 0; i < (int)equalities.size(); i++) {
            Expr eq = equalities[i].to_expr();
            for (const auto &v : eliminable_vars) {
                if (expr_uses_var(eq, v)) {
                    eqs_that_reference_var[v].push_back(i);
                }
            }
        }

        // The set of pairs of equations that share a common
        // eliminable variable
        set<pair<int, int>> has_common_variable;
        for (auto p : eqs_that_reference_var) {
            for (int i : p.second) {
                for (int j : p.second) {
                    has_common_variable.insert({i, j});
                }
            }
        }

        // Eliminate a variable
        for (int i = 0; i < (int)equalities.size(); i++) {
            if (equalities[i].num_vars == 0) {
                // We're not going to be able to find an
                // elimination from something with no naked vars.
                continue;
            }

            if (!interesting[i]) {
                // We've decided that this equation isn't one we want to mine.
                continue;
            }

            for (const auto &p : equalities[i].terms) {
                const Variable *var = p.first.as<Variable>();

                if (var) {

                    Expr rhs = 0, rhs_remainder = 0;
                    for (const auto &p2 : equalities[i].terms) {
                        // Every term on the RHS has to be either
                        // divisible by p.first, or in total
                        // bounded by p.first
                        if (p2.first.same_as(p.first)) {
                            // This is the LHS
                        } else if (p2.second % p.second == 0) {
                            rhs -= p2.first * (p2.second / p.second);
                        } else {
                            rhs_remainder -= p2.first * p2.second;
                        }
                    }

                    // We have:
                    // p.first * p.second == rhs * p.second + rhs_remainder

                    Simplify::ExprInfo remainder_bounds;
                    rhs_remainder = simplifier->mutate(rhs_remainder, &remainder_bounds);
                    rhs = simplifier->mutate(rhs, nullptr);

                    if (remainder_bounds.max_defined &&
                        remainder_bounds.max < std::abs(p.second) &&
                        remainder_bounds.min_defined &&
                        remainder_bounds.min > -std::abs(p.second)) {
                        // We have:
                        // p.first == rhs && 0 == rhs_remainder
                    } else {
                        // We don't have a substitution
                        continue;
                    }

                    if (expr_uses_var(rhs, var->name)) {
                        // Didn't successfully eliminate it - it
                        // still occurs inside a non-linearity on
                        // the right.
                        continue;
                    }

                    // Tell the simplifier that LHS == RHS. This
                    // may give it tighter bounds for the LHS
                    // variable based on what is currently known
                    // about the bounds of the RHS. This is the
                    // primary mechanism by which the simplifier
                    // instance learns things - not from the
                    // substitutions we actually perform, but from
                    // every potential substitution. Avoid telling
                    // the simplifier that x == x.
                    if (!equal(p.first, rhs)) {
                        simplifier->learn_true(p.first == rhs);
                    }

                    //debug(1) << "Attempting elimination of " << var->name << "\n";

                    /*
                    // We have a candidate for elimination. Rule
                    // out searching all equalities that don't
                    // share a common variable with this one,
                    // because we equally could have done any
                    // substitutions resulting from those first
                    // without affecting this substitution, and
                    // doing things in a canonical order avoids
                    // exploring the same states an exponential
                    // number of times.
                    for (int j = 0; j < (int)equalities.size(); j++) {
                        if (interesting[j]) {
                            interesting[j] = has_common_variable.count({i, j});
                        }
                    }
                    */

                    // If the RHS is just a constant or variable then
                    // we'll just greedily perform this elimination -
                    // there's no reason to need to backtrack on it,
                    // so nuke all other candidate children. There
                    // typically won't be any because x == y will sort
                    // to the front of the list of equalities.
                    bool greedy = false;
                    if (rhs.as<Variable>() || is_const(rhs)) {
                        greedy = true;
                        result.clear();
                    }

                    auto subs = [&](Expr e) {
                        e = substitute(var->name, rhs, e);
                        e = simplifier->mutate(e, nullptr);
                        return e;
                    };

                    // Make a child system with the substitution
                    // performed and this equality eliminated.
                    std::unique_ptr<System> new_system(new System(simplifier, p.first == rhs, id));
                    if (non_linear_term.defined()) {
                        new_system->add_term(subs(non_linear_term));
                    }
                    for (int j = 0; j < (int)equalities.size(); j++) {
                        if (i == j) {
                            // The equation we exploited to get
                            // the substitution gets reduced
                            // modulo the coefficient.
                            new_system->add_term(simplifier->mutate(rhs_remainder == 0, nullptr));
                            continue;
                        }
                        // In the other equations, we replace the variable with the right-hand-side
                        new_system->add_term(subs(equalities[j].to_expr()));
                    }
                    new_system->finalize();
                    result.emplace_back(std::move(new_system));

                    // No point considering further candidates if
                    // we're just doing a variable1 = variable2
                    // substitution.
                    if (greedy) {
                        return;
                    }
                }
            }
        }

        if (result.size() == old_size && !equalities.empty()) {
            debug(1) << "NO CHILDREN:\n";
            dump();
        }
    }
};

uint64_t System::id_counter = 0;

// Attempt to disprove a boolean expr by constructing a constraint
// system and performing a backtracking search over substitutions
// using beam search.
bool can_disprove(Expr e, int beam_size, std::set<Expr, IRDeepCompare> *implications = nullptr) {
    // e = common_subexpression_elimination(simplify(remove_likelies(e)));

    debug(1) << "*** Attempting disproof " << e << "\n";

    if (is_zero(e)) {
        // The simplifier was capable of doing the disproof by itself
        // using peephole rules alone. No need to continue.
        return true;
    }

    // Make a simplifier instance to hold all of our shared
    // knowledge, and construct the initial system of constraints
    // from the expression.
    Simplify simplifier(true, nullptr, nullptr);
    std::unique_ptr<System> system(new System(&simplifier, Expr(), 0));
    system->add_term(e);
    system->finalize();

    class FilterImplications : public IRVisitor {
        using IRVisitor::visit;

        void visit(const Variable *op) {
            // TODO: using var name prefixes here is a total hack
            if (starts_with(op->name, "c")) {
                return;
            } else {
                if (simplifier->bounds_and_alignment_info.contains(op->name)) {
                    auto info = simplifier->bounds_and_alignment_info.get(op->name);
                    if (info.min_defined || info.max_defined) {
                        return;
                    }
                }
            }
            debug(1) << "Rejecting due to "  << op->name << "\n";
            useful = false;
        }

    public:
        Simplify *simplifier;
        bool useful = true;
        FilterImplications(Simplify *s) : simplifier(s) {}
    };

    std::map<Expr, vector<int>, IRDeepCompare> local_implications;

    auto consider_implication = [&](const Expr &e, int id) {
        FilterImplications f(&simplifier);
        e.accept(&f);
        if (f.useful) {
            local_implications[e].push_back(id);
        } else {
            debug(1) << "Rejecting implication with unbounded terms: " << e << "\n";
        }
    };

    // Beam search time.
    std::deque<std::unique_ptr<System>> beam;
    beam.emplace_back(std::move(system));
    float last_complexity = 0;
    while (!beam.empty()) {
        // Take the best thing
        std::unique_ptr<System> next = std::move(beam.front());
        beam.pop_front();

        if (implications) {
            for (const auto &eq : next->equalities) {
                consider_implication(eq.to_expr(), next->id);
            }
            if (next->non_linear_term.defined()) {
                consider_implication(next->non_linear_term, next->id);
            }
        }

        //if (next->complexity() == last_complexity) continue;
        last_complexity = next->complexity();


          debug(1) << "Top of beam: " << next->complexity() << "\n";
          next->dump();

        if (next->infeasible()) {
            // We found that the initial constraint system
            // eventually implied a falsehood, so we successfully
            // disproved the original expression.
            if (implications) {
                implications->insert(const_false());
            }
            return true;
        }

        // Generate children
        next->make_children(beam);

        // Take the top beam_size results by sorting all the
        // children and then popping off the end. Not the most
        // efficient way to do it, but this is not the long pole
        // here.
        std::stable_sort(beam.begin(), beam.end(),
                         [&](const std::unique_ptr<System> &a,
                             const std::unique_ptr<System> &b) {
                             return a->complexity() < b->complexity();
                         });
        while ((int)beam.size() > beam_size) {
            beam.pop_back();
        }
    }

    auto get_bounds_from_info = [](const Simplify::ExprInfo &info) {
        Interval i = Interval::everything();
        if (info.min_defined) {
            i.min = (int)info.min;
        }
        if (info.max_defined) {
            i.max = (int)info.max;
        }
        return i;
    };

    if (implications) {
        Scope<Interval> scope;
        const auto &info = simplifier.bounds_and_alignment_info;
        for (auto it = info.cbegin(); it != info.cend(); ++it){
            Interval i = get_bounds_from_info(it.value());
            if (!starts_with(it.name(), "c")) {
                scope.push(it.name(), i);
            } else {
                Expr c = Variable::make(Int(32), it.name());
                if (i.has_upper_bound()) {
                    implications->insert(c <= i.max);
                }
                if (i.has_lower_bound()) {
                    implications->insert(i.min <= c);
                }
            }
        }

        // Mine the simplifier's list of memorized truth for symbolic
        // bounds on the remaining auxiliary variables.
        for (const Expr &t : simplifier.truths) {
            debug(1) << "Exploiting truth: " << t << "\n";
            if (const LT *lt = t.as<LT>()) {
                const Variable *va = lt->a.as<Variable>();
                const Variable *vb = lt->b.as<Variable>();
                if (va && scope.contains(va->name)) {
                    // The RHS may be a useful symbolic bound for the LHS
                    Interval rhs_bounds = bounds_of_expr_in_scope(lt->b, scope);
                    if (rhs_bounds.has_lower_bound()) {
                        Interval &i = scope.ref(va->name);
                        i.max = Interval::make_min(i.max, rhs_bounds.min - 1);
                    }
                }
                if (vb && scope.contains(vb->name)) {
                    // The LHS may be a useful symbolic bound for the RHS
                    Interval rhs_bounds = bounds_of_expr_in_scope(lt->a, scope);
                    if (rhs_bounds.has_upper_bound()) {
                        Interval &i = scope.ref(vb->name);
                        i.min = Interval::make_max(i.min, rhs_bounds.max + 1);
                    }
                }
            } else if (const LE *le = t.as<LE>()) {
                const Variable *va = le->a.as<Variable>();
                const Variable *vb = le->b.as<Variable>();
                if (va && scope.contains(va->name)) {
                    // The RHS may be a useful symbolic bound for the LHS
                    Interval rhs_bounds = bounds_of_expr_in_scope(le->b, scope);
                    if (rhs_bounds.has_lower_bound()) {
                        Interval &i = scope.ref(va->name);
                        i.max = Interval::make_min(i.max, rhs_bounds.min);
                    }
                }
                if (vb && scope.contains(vb->name)) {
                    // The LHS may be a useful symbolic bound for the RHS
                    Interval rhs_bounds = bounds_of_expr_in_scope(le->a, scope);
                    if (rhs_bounds.has_upper_bound()) {
                        Interval &i = scope.ref(vb->name);
                        i.min = Interval::make_max(i.min, rhs_bounds.max);
                    }
                }
            }
        }

        // Compute symbolic bounds, with the simplifier (and all its
        // accumulated knowledge) as a backstop. TODO: would be better
        // if we could pass a custom simplifier instance into
        // bounds_of_expr_in_scope.
        auto get_bounds = [&](const Expr &e) {
            Interval i = bounds_of_expr_in_scope(e, scope);
            Simplify::ExprInfo info;
            /*
            if (const Mul *mul = e.as<Mul>()) {
                debug(1) << "HI: bounds of product: " << e << "\n";
                simplifier.mutate(mul->a, &info);
                Interval mi = get_bounds_from_info(info);
                debug(1) << "LHS: " << mi.min << " ... " << mi.max << "\n";
                simplifier.mutate(mul->b, &info);
                mi = get_bounds_from_info(info);
                debug(1) << "RHS: " << mi.min << " ... " << mi.max << "\n";
            }
            */
            simplifier.mutate(e, &info);
            Interval si = get_bounds_from_info(info);
            if (!i.has_lower_bound() && si.has_lower_bound()) {
                i.min = si.min;
            }
            if (!i.has_upper_bound() && si.has_upper_bound()) {
                i.max = si.max;
            }
            return i;
        };

        auto add_intervals = [](const Interval &a, const Interval &b) -> Interval {
            if (a.is_empty()) {
                return b;
            }
            if (b.is_empty()) {
                return a;
            }
            Interval result = Interval::everything();
            if (a.has_lower_bound() && b.has_lower_bound()) {
                result.min = a.min + b.min;
            }
            if (a.has_upper_bound() && b.has_upper_bound()) {
                result.max = a.max + b.max;
            }
            return result;
        };

        // Now eliminate all the k's
        for (auto p : local_implications) {
            Expr m = p.first;
            debug(1) << "Local implication: " << m << "\n";
            m = simplify(m);
            debug(1) << "Simplify: " << m << "\n";
            if (const EQ *eq = m.as<EQ>()) {
                Expr a = eq->a, b = eq->b;
                vector<Expr> lhs = unpack_binary_op<Add>(a);
                vector<Expr> rhs = unpack_binary_op<Add>(b);
                // Every term must be bounded either above or below
                // for this to work out
                Interval lhs_range = Interval::nothing();
                Interval rhs_range = Interval::nothing();
                for (const Expr &e : lhs) {
                    Interval i = get_bounds(e);
                    lhs_range = add_intervals(lhs_range, i);
                    debug(1) << "Bounds of " << e << ": " << i.min << " ... " << i.max << "\n";
                }
                for (const Expr &e : rhs) {
                    Interval i = get_bounds(e);
                    rhs_range = add_intervals(rhs_range, i);
                    debug(1) << "Bounds of " << e << ": " << i.min << " ... " << i.max << "\n";
                }
                debug(1) << "Bounds of lhs: " << lhs_range.min << " ... " << lhs_range.max << "\n"
                         << "Bounds of rhs: " << rhs_range.min << " ... " << rhs_range.max << "\n";

                if (lhs_range.is_single_point() && rhs_range.is_single_point()) {
                    m = (lhs_range.min == rhs_range.min);
                } else {
                    m = const_true();
                    if (lhs_range.has_upper_bound() && rhs_range.has_lower_bound()) {
                        // Equality implies their ranges must overlap
                        m = (lhs_range.max >= rhs_range.min);
                    }
                    if (lhs_range.has_lower_bound() && rhs_range.has_upper_bound()) {
                        m = m && (lhs_range.min <= rhs_range.max);
                    }
                }
            } else {
                m = !and_condition_over_domain(!m, scope);
            }
            m = simplify(m);
            debug(1) << "Eliminate: " << m << "\n";
            if (!is_one(m)) {
                // We got something
                for (int id : p.second) {
                    debug(1) << "USEFUL LEAF: " << id << "\n";
                }
            }
            implications->insert(m);
        }
    }

    return false;
}

class RemoveMinMax : public IRMutator {
    using IRMutator::visit;

    IRMatcher::Wild<0> x;
    IRMatcher::Wild<1> y;
    IRMatcher::Wild<2> z;
    IRMatcher::Wild<3> w;

    enum class VarSign {
        Positive,
        NonNegative,
        NonPositive,
        Negative
    };

    Scope<VarSign> var_sign;

    bool is_non_negative(const Variable *v) const {
        if (!var_sign.contains(v->name)) {
            return false;
        }
        auto s = var_sign.get(v->name);
        return s == VarSign::Positive || s == VarSign::NonNegative;
    }

    bool is_non_positive(const Variable *v) const {
        if (!var_sign.contains(v->name)) {
            return false;
        }
        auto s = var_sign.get(v->name);
        return s == VarSign::Negative || s == VarSign::NonPositive;
    }

    Expr visit(const Add *op) override {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);
        auto rewrite = IRMatcher::rewriter(IRMatcher::add(a, b), op->type, op->type);

        if (rewrite(min(x, y) + z, select(x < y, x + z, y + z)) ||
            rewrite(z + min(x, y), select(x < y, z + x, z + y)) ||
            rewrite(max(x, y) + z, select(x < y, y + z, x + z)) ||
            rewrite(z + max(x, y), select(x < y, z + y, z + x)) ||
            rewrite(select(x, y, z) + w, select(x, y + w, z + w)) ||
            rewrite(w + select(x, y, z), select(x, w + y, w + z))) {
            return mutate(rewrite.result);
        } else {
            return a + b;
        }
    }

    Expr visit(const Sub *op) override {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);
        auto rewrite = IRMatcher::rewriter(IRMatcher::sub(a, b), op->type, op->type);

        if (rewrite(min(x, y) - z, select(x < y, x - z, y - z)) ||
            rewrite(z - min(x, y), select(x < y, z - x, z - y)) ||
            rewrite(max(x, y) - z, select(x < y, y - z, x - z)) ||
            rewrite(z - max(x, y), select(x < y, z - y, z - x)) ||
            rewrite(select(x, y, z) - w, select(x, y - w, z - w)) ||
            rewrite(w - select(x, y, z), select(x, w - y, w - z)) ||
            false) {
            return mutate(rewrite.result);
        } else {
            return a - b;
        }
    }

    Expr visit(const Mul *op) override {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);
        auto rewrite = IRMatcher::rewriter(IRMatcher::mul(a, b), op->type, op->type);
        const Variable *var_a = a.as<Variable>();
        const Variable *var_b = b.as<Variable>();

        if (var_b && !var_sign.contains(var_b->name)) {
            Expr zero = make_zero(b.type());
            // Break it into two cases with known sign
            Expr prod = a * b;
            return mutate(select(zero < b, prod,
                                 b < zero, prod,
                                 zero));
        } else if (var_a && !var_sign.contains(var_a->name)) {
            Expr zero = make_zero(a.type());
            // Break it into two cases with known sign
            Expr prod = a * b;
            return mutate(select(zero < a, prod,
                                 a < zero, prod,
                                 zero));
        } else if (
                   rewrite(min(x, y) * z, select(x < y, x * z, y * z)) ||
                   rewrite(z * min(x, y), select(x < y, z * x, z * y)) ||
                   rewrite(max(x, y) * z, select(x < y, y * z, x * z)) ||
                   rewrite(z * max(x, y), select(x < y, z * y, z * x)) ||
                   rewrite(select(x, y, z) * w, select(x, y * w, z * w)) ||
                   rewrite(w * select(x, y, z), select(x, w * y, w * z)) ||
                   rewrite((x + y) * z, x * z + y * z) ||
                   rewrite(z * (x + y), z * x + z * y) ||
                   false) {
            return mutate(rewrite.result);
        } else {
            return a * b;
        }
    }

    Expr visit(const Div *op) override {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);
        auto rewrite = IRMatcher::rewriter(IRMatcher::div(a, b), op->type, op->type);
        const Variable *var_b = b.as<Variable>();
        const Variable *var_a = a.as<Variable>();

        if (var_b && !var_sign.contains(var_b->name)) {
            Expr zero = make_zero(b.type());
            // Break it into two cases with known sign
            Expr ratio = a / b;
            return mutate(select(zero < b, ratio,
                                 b < zero, ratio,
                                 zero)); // This case is in fact unreachable
        } else if (var_a && !var_sign.contains(var_a->name)) {
            Expr zero = make_zero(a.type());
            // Break it into two cases with known sign
            Expr ratio = a / b;
            return mutate(select(zero < a, ratio,
                                 a < zero, ratio,
                                 zero)); // This case is in fact unreachable
        } else if (rewrite(min(x, y) / z, select(x < y, x / z, y / z)) ||
            rewrite(z / min(x, y), select(x < y, z / x, z / y)) ||
            rewrite(max(x, y) / z, select(y < x, x / z, y / z)) ||
            rewrite(z / max(x, y), select(y < x, z / x, z / y)) ||
            rewrite(select(x, y, z) / w, select(x, y / w, z / w)) ||
            rewrite(w / select(x, y, z), select(x, w / y, w / z))) {
            return mutate(rewrite.result);
        } else {
            return a / b;
        }
    }

    Expr visit(const LT *op) override {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);
        auto rewrite = IRMatcher::rewriter(IRMatcher::lt(a, b), op->type, a.type());

        if (rewrite(min(x, y) < z, (x < z) || (y < z)) ||
            rewrite(z < min(x, y), (z < x) && (z < y)) ||
            rewrite(max(x, y) < z, (x < z) && (y < z)) ||
            rewrite(z < max(x, y), (z < x) || (z < y)) ||
            //            rewrite(select(x, y, z) < w, (x && (y < w)) || (!x && (z < w))) ||
            //rewrite(w < select(x, y, z), (x && (w < y)) || (!x && (w < z)))) {
            rewrite(select(x, y, z) < w, select(x, y < w, z < w)) ||
            rewrite(w < select(x, y, z), select(x, w < y, w < z)) ||
            false) {
            return mutate(rewrite.result);
        } else {
            return a < b;
        }
    }

    Expr visit(const LE *op) override {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);
        auto rewrite = IRMatcher::rewriter(IRMatcher::le(a, b), op->type, a.type());

        if (rewrite(min(x, y) <= z, (x <= z) || (y <= z)) ||
            rewrite(z <= min(x, y), (z <= x) && (z <= y)) ||
            rewrite(max(x, y) <= z, (x <= z) && (y <= z)) ||
            rewrite(z <= max(x, y), (z <= x) || (z <= y)) ||
            //            rewrite(select(x, y, z) <= w, x && (y <= w) || !x && (z <= w)) ||
            //            rewrite(w <= select(x, y, z), x && (w <= y) || !x && (w <= z)) ||
            rewrite(select(x, y, z) <= w, select(x, y <= w, z <= w)) ||
            rewrite(w <= select(x, y, z), select(x, w <= y, w <= z)) ||
            false) {
            return mutate(rewrite.result);
        } else {
            return a <= b;
        }
    }

    Expr visit(const NE *op) override {
        if (!op->a.type().is_bool()) {
            return mutate((op->a < op->b) || (op->b < op->a));
        } else {
            return mutate((op->a && !op->b) || (!op->a && op->b));
        }
    }

    Expr visit(const Select *op) override {
        if (const LT *lt = op->condition.as<LT>()) {
            const Variable *var_a = lt->a.as<Variable>();
            const Variable *var_b = lt->b.as<Variable>();
            if (is_zero(lt->a) && var_b) {
                if (var_sign.contains(var_b->name)) {
                    auto s = var_sign.get(var_b->name);
                    if (s == VarSign::Positive) {
                        return mutate(op->true_value);
                    } else if (s == VarSign::Negative ||
                               s == VarSign::NonPositive) {
                        return mutate(op->false_value);
                    }
                }
                Expr cond = mutate(op->condition);
                Expr true_value, false_value;
                {
                    ScopedBinding<VarSign> bind(var_sign, var_b->name, VarSign::Positive);
                    true_value = mutate(op->true_value);
                }
                {
                    ScopedBinding<VarSign> bind(var_sign, var_b->name, VarSign::NonPositive);
                    false_value = mutate(op->false_value);
                }
                return select(cond, true_value, false_value);
            } else if (is_zero(lt->b) && var_a) {
                if (var_sign.contains(var_a->name)) {
                    auto s = var_sign.get(var_a->name);
                    if (s == VarSign::Negative) {
                        return mutate(op->true_value);
                    } else if (s == VarSign::Positive ||
                               s == VarSign::NonNegative) {
                        return mutate(op->false_value);
                    }
                }
                Expr cond = mutate(op->condition);
                Expr true_value, false_value;
                {
                    ScopedBinding<VarSign> bind(var_sign, var_a->name, VarSign::Negative);
                    true_value = mutate(op->true_value);
                }
                {
                    ScopedBinding<VarSign> bind(var_sign, var_a->name, VarSign::NonNegative);
                    false_value = mutate(op->false_value);
                }
                return select(cond, true_value, false_value);
            }
        }
        return IRMutator::visit(op);
    }
};

class FindAllSelectConditions : public IRVisitor {
    void visit(const Select *op) override {
        cases.insert(op->condition);
        IRVisitor::visit(op);
    }
public:
    std::set<Expr, IRDeepCompare> cases;
};

class RemoveSelect : public IRMutator {
    using IRMutator::visit;
    Expr visit(const Select *op) override {
        if (!op->type.is_bool()) {
            return IRMutator::visit(op);
        }
        return (mutate(op->condition && op->true_value) ||
                mutate(!op->condition && op->false_value));
    }
};

class MoveNegationInnermost : public IRMutator {
    using IRMutator::visit;

    Expr visit(const Not *op) override {
        if (const And *and_a = op->a.as<And>()) {
            return mutate(!and_a->a) || mutate(!and_a->b);
        } else if (const Or *or_a = op->a.as<Or>()) {
            return mutate(!or_a->a) && mutate(!or_a->b);
        } else if (const Not *not_a = op->a.as<Not>()) {
            return mutate(not_a->a);
        } else if (const LT *lt = op->a.as<LT>()) {
            return mutate(lt->b <= lt->a);
        } else if (const LE *le = op->a.as<LE>()) {
            return mutate(le->b < le->a);
        } else {
            return IRMutator::visit(op);
        }
    }
};

class ToDNF : public IRMutator {
    using IRMutator::visit;

    Expr visit(const And *op) override {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);
        vector<Expr> as = unpack_binary_op<Or>(a);
        vector<Expr> bs = unpack_binary_op<Or>(b);
        set<Expr, IRDeepCompare> result;
        for (Expr a1 : as) {
            for (Expr b1 : bs) {
                auto a_clauses = unpack_binary_op<And>(a1);
                auto b_clauses = unpack_binary_op<And>(b1);
                set<Expr, IRDeepCompare> both;
                both.insert(a_clauses.begin(), a_clauses.end());
                both.insert(b_clauses.begin(), b_clauses.end());
                result.insert(pack_binary_op<And>(both));
            }
        }
        return pack_binary_op<Or>(result);
    }
};

class ToCNF : public IRMutator {
    using IRMutator::visit;

    Expr visit(const Or *op) override {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);
        vector<Expr> as = unpack_binary_op<And>(a);
        vector<Expr> bs = unpack_binary_op<And>(b);
        set<Expr, IRDeepCompare> result;
        for (Expr a1 : as) {
            for (Expr b1 : bs) {
                auto a_clauses = unpack_binary_op<Or>(a1);
                auto b_clauses = unpack_binary_op<Or>(b1);
                set<Expr, IRDeepCompare> both;
                both.insert(a_clauses.begin(), a_clauses.end());
                both.insert(b_clauses.begin(), b_clauses.end());
                result.insert(pack_binary_op<Or>(both));
            }
        }
        return pack_binary_op<And>(result);
    }
};

class ExtractCase : public IRMutator {
    using IRMutator::visit;

    Expr visit(const Select *op) override {
        if (equal(op->condition, c)) {
            if (val) {
                return mutate(op->true_value);
            } else {
                return mutate(op->false_value);
            }
        } else {
            return IRMutator::visit(op);
        }
    }

    Expr c;
    bool val;
public:
    ExtractCase(Expr c, bool val) : c(c), val(val) {}
};

class ConvertRoundingToMod : public IRMutator {
    using IRMutator::visit;

    Expr visit(const Mul *op) {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);

        const Div *d = a.as<Div>();
        if (d && equal(d->b, b)) {
            // Euclidean identity says: (a/b)*b + a % b == a. So:
            // (x / y) * y -> x - x % y
            return d->a - d->a % d->b;
        } else {
            return a * b;
        }
    }
};

// Take a boolean expression with min/max/select in it, and reduce it
// to a big disjunction of inequalities intead.
vector<Expr> remove_min_max_select(Expr e) {
    // First turn min/max into select
    RemoveMinMax b;
    e = b.mutate(e);
    vector<Expr> pieces{e};
    // Then find all the select conditions
    FindAllSelectConditions finder;
    e.accept(&finder);

    // Fork the expr into cases according to the truth values of the
    // select conditions.
    for (auto c : finder.cases) {
        vector<Expr> pending;
        pending.swap(pieces);
        while (!pending.empty()) {
            Expr next = pending.back();
            pending.pop_back();
            // Fork everything in pending according to the case being true or false
            Expr true_case = ExtractCase(c, true).mutate(next);
            if (equal(true_case, next)) {
                // Condition did not occur
                pieces.push_back(next);
            } else {
                Expr false_case = ExtractCase(c, false).mutate(next);
                pieces.push_back(c && true_case);
                pieces.push_back(!c && false_case);
            }
        }
    }

    for (auto &p : pieces) {
        // Remove any remaining selects
        p = RemoveSelect().mutate(p);
        // Apply DeMorgan's law to move not operations innermost
        p = MoveNegationInnermost().mutate(p);
    }

    return pieces;
}

bool can_disprove_nonconvex(Expr e, int beam_size, Expr *implication) {

    debug(1) << "Attempting to disprove non-convex expression: " << e << "\n";

    // Canonicalize >, >=, and friends
    e = simplify(e);

    // Break it into convex pieces, and disprove every piece
    debug(1) << "Simplified: " << e << "\n";

    auto pieces = remove_min_max_select(e);

    for (auto &p : pieces) {
        // Distribute and over or.
        p = ToDNF().mutate(p);
        e = pack_binary_op<Or>(pieces);
        pieces = unpack_binary_op<Or>(e);
    }

    debug(1) << "In DNF form:\n";
    int i = 0;
    for (auto p : pieces) {
        debug(1) << (++i) << ") " << p << "\n";
    }

    // Simplify each piece.
    debug(1) << "Simplify each piece:\n";
    std::set<Expr, IRDeepCompare> simplified_pieces;
    i = 0;
    for (const Expr &p : pieces) {
        Simplify simplifier(true, nullptr, nullptr);
        std::set<Expr, IRDeepCompare> simplified_clauses;
        auto clauses = unpack_binary_op<And>(p);
        for (Expr c : clauses) {
            // debug(0) << "Clause: " << c << "\n";
            c = simplifier.mutate(c, nullptr);
            if (is_one(c)) {
                continue;
            }
            simplifier.learn_true(c);
            if (is_zero(c)) {
                simplified_clauses.clear();
            }
            simplified_clauses.insert(c);
            // debug(0) << "Clause: " << c << "\n";
            if (is_zero(c)) {
                break;
            }
        }
        Expr result = pack_binary_op<And>(simplified_clauses);
        if (is_zero(result)) {
            debug(1) << (++i) << ") empty\n";
        } else if (simplified_pieces.count(result)) {
            debug(1) << (++i) << ") duplicate\n";
        } else {
            debug(1) << (++i) << ") " << result << "\n";
            simplified_pieces.insert(result);
        }
    }

    if (implication) {
        // We're going to or together a term from each convex piece.
        *implication = const_false();
    }

    bool failed = false;

    for (auto p : simplified_pieces) {
        set<Expr, IRDeepCompare> implications;

        debug(1) << "Attempting to disprove non-trivial term: " << p << "\n";
        if (can_disprove(p, beam_size, &implications)) {
            debug(1) << "Success!\n";
        } else {
            debug(1) << "Failure\n";
            failed = true;
        }

        if (implication) {
            Expr m = pack_binary_op<And>(implications);
            if (m.defined()) {
                m = simplify(m);
                debug(1) << "Piece: " << p << "\n"
                         << "implies: " << m << "\n";
                *implication = *implication || m;
            } else {
                debug(1) << "Learned nothing from piece: " << p << "\n";
                *implication = const_true();
            }
        }
    }

    if (implication) {
        debug(1) << "Unsimplified combined implication: " << *implication << "\n";
        *implication = simplify(*implication);
    }

    return !failed;
}

Expr synthesize_predicate(const Expr &lhs,
                          const Expr &rhs,
                          const vector<map<string, Expr>> &examples,
                          map<string, Expr> *binding) {

    Expr assumption = const_true();
    Expr to_prove = lhs == rhs;
    Expr m;

    assumption = simplify(assumption);
    debug(1) << can_disprove_nonconvex(assumption && !to_prove, 1024, &m);

    debug(1) << "\nImplication: " << m << "\n";

    class NormalizePrecondition : public IRMutator {
        using IRMutator::visit;
        Expr visit(const Not *op) override {
            if (const Or *o = op->a.as<Or>()) {
                return mutate(!o->a && !o->b);
            } else if (const And *o = op->a.as<And>()) {
                return mutate(!o->a || !o->b);
            } else if (const LT *l = op->a.as<LT>()) {
                return mutate(l->b <= l->a);
            } else if (const LE *l = op->a.as<LE>()) {
                return mutate(l->b < l->a);
            } else if (const EQ *eq = op->a.as<EQ>()) {
                return mutate(eq->a < eq->b || eq->b < eq->a);
            } else if (const NE *ne = op->a.as<NE>()) {
                return mutate(ne->a == ne->b);
            } else if (const Select *s = op->a.as<Select>()) {
                return mutate(select(s->condition, !s->true_value, !s->false_value));
            } else {
                return IRMutator::visit(op);
            }
        }

        Expr visit(const And *op) override {
            auto v = unpack_binary_op<And>(Expr(op));
            for (auto &t : v) {
                t = mutate(t);
            }
            set<Expr, IRDeepCompare> terms(v.begin(), v.end());
            return pack_binary_op<And>(terms);
        }

        Expr visit(const Or *op) override {
            auto v = unpack_binary_op<Or>(Expr(op));
            for (auto &t : v) {
                t = mutate(t);
            }
            set<Expr, IRDeepCompare> terms(v.begin(), v.end());
            return pack_binary_op<Or>(terms);
        }

        Expr visit(const LT *op) override {
            if (is_const(op->b)) {
                // x < 0 -> x <= -1
                return mutate(op->a <= simplify(op->b - 1));
            } else if (is_const(op->a)) {
                // 0 < x -> 1 <= x
                return mutate(simplify(op->a + 1) <= op->b);
            } else {
                return IRMutator::visit(op);
            }
        }

        Expr visit(const LE *op) override {
            const Min *min_a = op->a.as<Min>();
            const Min *min_b = op->b.as<Min>();
            const Max *max_a = op->a.as<Max>();
            const Max *max_b = op->b.as<Max>();
            if (is_const(op->b) && can_prove(op->a != op->b)) {
                return mutate(op->a <= simplify(op->b - 1));
            } else if (is_const(op->a) && can_prove(op->a != op->b)) {
                return mutate(simplify(op->a + 1) <= op->b);
            } else if (min_a) {
                return mutate(min_a->a <= op->b || min_a->b <= op->b);
            } else if (max_a) {
                return mutate(max_a->a <= op->b && max_a->b <= op->b);
            } else if (min_b) {
                return mutate(op->a <= min_b->a && op->a <= min_b->b);
            } else if (max_b) {
                return mutate(op->a <= max_b->a || op->a <= max_b->b);
            } else {
                return IRMutator::visit(op);
            }
        }

        Expr visit(const EQ *op) override {
            Expr a = mutate(op->a);
            Expr b = mutate(op->b);
            if (IRDeepCompare()(a, b)) {
                return a == b;
            } else {
                return b == a;
            }
        }

        Expr visit(const NE *op) override {
            Expr a = mutate(op->a);
            Expr b = mutate(op->b);
            if (is_const(a) || is_const(b)) {
                return mutate(a < b) || mutate(b < a);
            } else if (IRDeepCompare()(a, b)) {
                return a != b;
            } else {
                return b != a;
            }
        }

        Internal::Simplify *simplifier;

        bool can_prove(Expr e) const {
            return is_one(simplifier->mutate(e, nullptr));
        }
    public:
        NormalizePrecondition(Internal::Simplify *s) : simplifier(s) {}
    };

    class ImplicitAssumptions : public IRVisitor {
        void visit(const Mul *op) override {
            const Variable *v = op->b.as<Variable>();
            if (v && v->name[0] == 'c') {
                result.push_back(op->b != 0);
                result.push_back(op->b != 1);
                result.push_back(op->b <= -1 || 1 <= op->b);
                result.push_back(1 <= op->b || op->b <= -1);
            }
            IRVisitor::visit(op);
        }
        void visit(const Div *op) override {
            const Variable *v = op->b.as<Variable>();
            if (v && v->name[0] == 'c') {
                result.push_back(op->b != 0);
                result.push_back(op->b != 1);
                result.push_back(op->b <= -1 || 1 <= op->b);
                result.push_back(1 <= op->b || op->b <= -1);
            }
            IRVisitor::visit(op);
        }
    public:
        vector<Expr> result;
    } implicit_assumptions;
    lhs.accept(&implicit_assumptions);
    Expr implicit_assumption = pack_binary_op<And>(implicit_assumptions.result);

    // Simplify implication using the assumption
    Simplify simplifier(true, nullptr, nullptr);
    simplifier.learn_true(implicit_assumption);

    Expr precondition = simplify(assumption && !m);
    debug(1) << "Precondition: " << precondition << "\n";
    // precondition = ToCNF().mutate(precondition);
    // debug(1) << "CNF: " << precondition << "\n";
    NormalizePrecondition normalizer(&simplifier);
    precondition = normalizer.mutate(precondition);
    //precondition = ToDNF().mutate(precondition);
    debug(1) << "Normalized: " << precondition << "\n";

    // Check satisfiability with z3
    {
        map<string, Expr> bindings;
        auto z3_result = satisfy(precondition, &bindings);

        // Early-out
        if (z3_result == Z3Result::Unsat) {
            return const_false();
        }
    }

    // We probably have a big conjunction. Use each term in it to
    // simplify all other terms, to reduce the number of
    // overlapping conditions.
    vector<Expr> clause_vec = unpack_binary_op<And>(precondition);
    if (0) {
        for (auto &c : clause_vec) {
            debug(0) << "A: " << c << "\n";
            c = pack_binary_op<Or>(remove_min_max_select(c));
            debug(0) << "B: " << c << "\n";
            c = ToCNF().mutate(c);
            debug(0) << "C: " << c << "\n";
        }
        Expr e = pack_binary_op<And>(clause_vec);
        clause_vec = unpack_binary_op<And>(e);
    }

    set<Expr, IRDeepCompare> clauses(clause_vec.begin(), clause_vec.end());

    debug(1) << "Clauses before CNF simplifications:\n";
    for (auto &c : clauses) {
        debug(1) << " " << c << "\n\n";
    }

    // We end up with a lot of pairs of clauses of the form:
    // (a || b) && ... && (a || !b).
    // We can simplify these to just a.
    vector<set<Expr, IRDeepCompare>> cnf;
    for (const auto &c : clauses) {
        vector<Expr> terms = unpack_binary_op<Or>(c);
        set<Expr, IRDeepCompare> term_set(terms.begin(), terms.end());
        cnf.push_back(std::move(term_set));
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < cnf.size() && !changed; i++) {
            auto &c1 = cnf[i];
            if (c1.size() == 1 && is_one(*c1.begin())) continue;
            for (size_t j = i+1; j < cnf.size() && !changed; j++) {
                auto &c2 = cnf[j];
                if (c2.size() == 1 && is_one(*c2.begin())) continue;

                debug(1) << "Considering pair:\n"
                         << " c1 = " << pack_binary_op<Or>(c1) << "\n"
                         << " c2 = " << pack_binary_op<Or>(c2) << "\n";

                // (A || B) && (A || C) == (A || (B && C)) whenever (B && C) usefully simplifies
                // check if (c1 - c2) and (c2 - c1) is false and if so replace with intersection
                set<Expr, IRDeepCompare> c2_minus_c1, c1_minus_c2;
                std::set_difference(c1.begin(), c1.end(), c2.begin(), c2.end(),
                                    std::inserter(c1_minus_c2, c1_minus_c2.end()),
                                    IRDeepCompare{});
                std::set_difference(c2.begin(), c2.end(), c1.begin(), c1.end(),
                                    std::inserter(c2_minus_c1, c2_minus_c1.end()),
                                    IRDeepCompare{});
                if (c1_minus_c2.empty()) {
                    debug(1) << " c1 && c2 -> c1\n";
                    // A && (A || B) -> A && true
                    c2.clear();
                    c2.insert(const_true());
                    changed = true;
                } else if (c2_minus_c1.empty()) {
                    debug(1) << " c1 && c2 -> c2\n";
                    c1.clear();
                    c1.insert(const_true());
                    changed = true;
                } else {
                    Expr a = pack_binary_op<Or>(c2_minus_c1);
                    Expr b = pack_binary_op<Or>(c1_minus_c2);
                    Expr a_and_b = normalizer.mutate(simplify(a && b));
                    if (a_and_b.as<And>() == nullptr) {
                        set<Expr, IRDeepCompare> c1_and_c2;
                        std::set_intersection(c1.begin(), c1.end(), c2.begin(), c2.end(),
                                              std::inserter(c1_and_c2, c1_and_c2.end()),
                                              IRDeepCompare{});
                        c1.swap(c1_and_c2);
                        if (!is_zero(a_and_b)) {
                            c1.insert(a_and_b);
                        }
                        debug(1) << " c1 && c2 -> " << pack_binary_op<Or>(c1) << "\n";
                        c2.clear();
                        c2.insert(const_true());
                        changed = true;
                    }
                }
            }
        }
    }

    clauses.clear();
    for (auto &c : cnf) {
        if (c.empty()) {
            // An empty disjunction is false, making the whole thing
            // false.
            return const_false();
        }
        Expr clause = pack_binary_op<Or>(c);
        if (is_one(clause)) {
            continue;
        }
        clauses.insert(clause);
    }

    debug(1) << "Clauses after CNF simplifications:\n";
    for (auto &c : clauses) {
        debug(1) << " " << c << "\n";
    }

    for (auto p : find_vars(rhs)) {
        string v = p.first;
        if (expr_uses_var(lhs, v)) {
            continue;
        }
        debug(1) << "implicit var: " << v << "\n";
        set<Expr, IRDeepCompare> upper_bound, lower_bound;
        for (Expr c : clauses) {
            if (!expr_uses_var(c, v)) {
                continue;
            }
            debug(1) << "Considering " << c << "\n";
            Expr result = solve_expression(c, v).result;
            debug(1) << "Solved clause: " << result << "\n";

            if (const LT *lt = result.as<LT>()) {
                result = (lt->a <= lt->b - 1);
            } else if (const GT *gt = result.as<GT>()) {
                result = (gt->a >= gt->b + 1);
            }

            const EQ *eq = result.as<EQ>();
            const LE *le = result.as<LE>();
            const GE *ge = result.as<GE>();
            Expr a, b;
            if (eq) {
                a = eq->a;
                b = eq->b;
            } else if (le) {
                a = le->a;
                b = le->b;
            } else if (ge) {
                a = ge->a;
                b = ge->b;
            } else {
                continue;
            }

            const Variable *var_a = a.as<Variable>();
            const Mul *mul_a = a.as<Mul>();
            if (mul_a) {
                var_a = mul_a->a.as<Variable>();
            }

            if (!var_a || var_a->name != v) {
                continue;
            }

            if (mul_a && !is_one(simplifier.mutate(mul_a->b >= 0, nullptr))) {
                // TODO: could also do something for provably negative
                // multipliers.
                continue;
            }

            if (mul_a && le) {
                b = b / mul_a->b;
            } else if (mul_a && ge) {
                b = (b + mul_a->b - 1) / mul_a->b;
            } else if (mul_a && eq) {
                b = b / mul_a->b;
            }

            if (expr_uses_var(b, v)) {
                continue;
            }

            if (eq || le) {
                upper_bound.insert(b);
            }
            if (eq || ge) {
                lower_bound.insert(b);
            }
        }

        // Now we need to pick a value for the implicit var. It can be
        // anything, because we'll substitute it back into the
        // predicate. So if we pick something bad, the predicate will
        // simply not match (as the implicit condition will not hold),
        // and no harm done. We'll use the max of the lower bounds and
        // the min of the upper bounds.

        if (upper_bound.empty() && lower_bound.empty()) {
            debug(0) << "In synthesizing predicate for " << lhs << " == " << rhs << "\n"
                     << "with implicit predicate: " << pack_binary_op<And>(clauses) << "\n"
                     << "Failed to bound implicit var " << v << "\n";
            return const_false();
        }

        if (!upper_bound.empty()) {
            lower_bound.insert(pack_binary_op<Min>(upper_bound));
        }
        Expr proposal = pack_binary_op<Max>(lower_bound);
        proposal = simplify(proposal);

        // Eliminate this variable from all existing bindings, and
        // from all future clauses
        for (auto &p : *binding) {
            p.second = substitute(v, proposal, p.second);
        }

        set<Expr, IRDeepCompare> new_clauses;
        for (Expr c : clauses) {
            new_clauses.insert(substitute(v, proposal, c));
        }
        clauses.swap(new_clauses);

        (*binding)[v] = proposal;
    }

    // Replace LHS constant wildcards with actual constants where possible
    vector<Expr> new_clauses;
    for (Expr c : clauses) {
        c = substitute(*binding, c);
        const EQ *eq = c.as<EQ>();
        if (!eq) {
            new_clauses.push_back(c);
            continue;
        }
        const Variable *var_a = eq->a.as<Variable>();
        const Variable *var_b = eq->b.as<Variable>();
        if (!var_a || !(var_b || is_const(eq->b))) {
            new_clauses.push_back(c);
            continue;
        }

        for (auto &p : *binding) {
            p.second = substitute(var_a->name, eq->b, p.second);
        }

        for (Expr &c2 : new_clauses) {
            c2 = substitute(var_a->name, eq->b, c2);
        }

        (*binding)[var_a->name] = eq->b;
    }

    if (new_clauses.empty()) {
        precondition = const_true();
    } else {
        precondition = pack_binary_op<And>(new_clauses);
    }
    precondition = simplifier.mutate(precondition, nullptr);

    debug(1) << "Projected out: " << precondition << "\n";

    debug(0) << common_subexpression_elimination(precondition) << "\n";

    // Now super-simplify it
    /*
    for (int i = 0; i < 5; i++) {
        debug(1) << "Attempting super simplification at size " << i << "\n";
        Expr ss = super_simplify(precondition, i);
        if (ss.defined()) {
            precondition = ss;
            break;
        }
    }
    */

    debug(0) << "Precondition " << precondition << "\n"
             << "implies " << to_prove << "\n";

    return precondition;
}
