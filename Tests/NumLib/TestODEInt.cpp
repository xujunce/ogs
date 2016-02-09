#include <gtest/gtest.h>
#include <logog/include/logog.hpp>

#include <fstream>
#include <memory>
#include <typeinfo>

#include "TimeLoop.h"
#include "Odes.h"
#include "BaseLib/BuildInfo.h"


template<NonlinearSolverTag NLTag>
class TestOutput
{
public:
    template<typename Ode>
    void run_test(Ode& ode, ITimeDiscretization& timeDisc)
    {
        auto const t_end = OdeTraits<Ode>::t_end;
        auto const t0    = OdeTraits<Ode>::t0;
        run_test<Ode>(ode, timeDisc, (t_end-t0)/10.0); // by default make 10 timesteps
    }

    template<typename Ode>
    void run_test(Ode& ode, ITimeDiscretization& timeDisc, const double delta_t)
    {
        init_file(ode, timeDisc, delta_t);

        TimeDiscretizedODESystem<NLTag> ode_sys(ode, timeDisc);
        TimeLoop<NLTag> loop(ode_sys, _nonlinear_solver);

        const double t0    = OdeTraits<Ode>::t0;
        const double t_end = OdeTraits<Ode>::t_end;

        // initial condition
        Vector x0;
        OdeTraits<Ode>::setIC(x0);

        write(t0, x0, x0);

        auto cb = [this](const double t, Vector const& x) {
            loopCallback<Ode>(t, x);
        };
        loop.loop(t0, x0, t_end, delta_t, cb);
    }

private:
    template<typename Ode, typename TimeDisc>
    void init_file(Ode const& ode, TimeDisc const& timeDisc, const double delta_t)
    {
        std::string path(BaseLib::BuildInfo::tests_tmp_path + "ODEInt_");
        path += typeid(ode).name();
        path += "_";
        path += typeid(timeDisc).name();
        path += "_";

        switch (NLTag) {
        case NonlinearSolverTag::Picard: path += "Picard"; break;
        case NonlinearSolverTag::Newton: path += "Newton"; break;
        }

        path += "_" + std::to_string(delta_t);
        path += ".csv";

        _file.reset(new std::ofstream(path));
        _file->precision(15);
    }

    void write(double const t, Vector const& x_num, Vector const& x_ana)
    {
        *_file << t;
        for (IndexType i=0; i<x_ana.size(); ++i) *_file << '\t' << x_ana[i];
        for (IndexType i=0; i<x_num.size(); ++i) *_file << '\t' << x_num[i];
        *_file << "\n";
    }

    template<typename Ode>
    void loopCallback(const double t, Vector const& x)
    {
        write(t, x, OdeTraits<Ode>::solution(t));
    }

    std::unique_ptr<std::ofstream> _file;

    const double _tol = 1e-8;
    const unsigned _maxiter = 10;
    NonlinearSolver<NLTag> _nonlinear_solver = NonlinearSolver<NLTag>(_tol, _maxiter);
};


TEST(NumLibODEInt, Ode1_BwdEuler)
{
    auto const NLTag = NonlinearSolverTag::Picard;
    using TimeDisc = BackwardEuler;

    Ode1 ode;
    TimeDisc timeDisc;

    TestOutput<NLTag> test;
    test.run_test(ode, timeDisc);
}


TEST(NumLibODEInt, Ode1_FwdEuler)
{
    auto const NLTag = NonlinearSolverTag::Newton;
    using TimeDisc = ForwardEuler;

    Ode1 ode;
    TimeDisc timeDisc;

    TestOutput<NLTag> test;
    test.run_test(ode, timeDisc);
}


TEST(NumLibODEInt, Ode2_BwdEuler)
{
    auto const NLTag = NonlinearSolverTag::Newton;
    using TimeDisc = BackwardEuler;

    Ode2 ode;
    TimeDisc timeDisc;

    TestOutput<NLTag> test;
    test.run_test(ode, timeDisc);
}


TEST(NumLibODEInt, Ode2_CrankNicolson)
{
    auto const NLTag = NonlinearSolverTag::Newton;
    using TimeDisc = CrankNicolson;

    Ode2 ode;
    TimeDisc timeDisc(0.5);

    TestOutput<NLTag> test;
    test.run_test(ode, timeDisc);
}


TEST(NumLibODEInt, Ode2_BDF)
{
    auto const NLTag = NonlinearSolverTag::Newton;
    using TimeDisc = BackwardDifferentiationFormula;

    Ode2 ode;
    TimeDisc timeDisc(3);

    TestOutput<NLTag> test;
    test.run_test(ode, timeDisc);
}


