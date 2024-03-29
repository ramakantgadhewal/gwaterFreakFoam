/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2011-2016 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

Application
    darcyFoam

Description
    Stationary solver for incompressible single-phase flow in porous medium

Developers
    Pierre Horgue then Olivier Atteia to integrate phreeqc

\*---------------------------------------------------------------------------*/
//#include <cmath.h>
//#include <iomanip> //NB when in < > don't add the .h
#include <chrono>  // for high_resolution_clock
#include "fvCFD.H"
#include "incompressiblePhase.H"
#include "inletOutletFvPatchField.H"
#include "volFields.H"
#include "fvPatchFieldMapper.H"
#include "fvOptions.H"
#include "simpleControl.H"

#include "phreeqc/initPhreeqc.H"
std::vector<double> a(12,0.);
std::vector<double> c_ph,gm_ph,gvol;
float atmPa=101325.;float vmw,Cgtot;
int i,j,iw;
my_phq freak; //need to be here to be availabel for every chem condition

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

using namespace Foam;

int main(int argc, char *argv[])
{
	//init openFoam
    #include "setRootCase.H"
    #include "createTime.H"
    #include "createMesh.H"
    #include "readGravitationalAcceleration.H"
    #include "createFields.H"
	#include "readPicardControls.H"
    simpleControl simple(mesh);
	#include "createThetaFields.H"
	#include "create2phaseFields.H"
	
	//##############  phreeqc intiialisation for solutions and gases
	std::ifstream inputRactive{cur_dir+"/constant/options/ractive" }; // version 0 shall contain 0 for inactive and 1 for active reaction cell
	std::vector<int>  ractive = {std::istream_iterator<int>{inputRactive}, {}};
	std::ifstream inputInit{cur_dir/"phqinit.txt"};
	std::vector<int> ph_init{std::istream_iterator<int>{inputInit}, {}}; //for (int i=0; i<7;i++){Info << "init nb "<< ph_data[i] << endl;}
	nxyz=ph_init[0];ph_ncomp=ph_init[1];ph_gcomp=ph_init[2];ph_nsolu=ph_init[3];
	freak.setDB(cur_dir/"phreeqc.dat");
	//make a first init to calculate the inital solutions (poro required for the initial solutions)
	freak.setData(ph_init);
	std::vector<double> poro(nxyz,0.25);
	for (i=0;i<nxyz;i++) {poro[i] = eps[i];Info<<"init eps "<<eps[i]<<endl;}
	freak.setPoro(poro);
	freak.setChemFile(cur_dir/"initChem.pqi"); //Info << "initCh read " << endl;
	freak.init(); //Info << "nxyz " << nxyz << endl;
	
	//################ writes the initial solutions and gases to files
	std::ofstream outFile(cur_dir/"constant/options/solutions");
	std::vector<float> solu_conc(ph_nsolu*ph_ncomp,0.);Info << "nsolu "<<ph_nsolu << " ncomp "<< ph_ncomp <<endl;
	for (i=0;i<ph_nsolu;i++) // solu number
		{ for (j=0;j<ph_ncomp;j++) // component number
			{
			float a = freak.c[j*ph_nsolu+i];
			outFile << a << "\n"; solu_conc[i*ph_ncomp+j] = a;
			} 
		}
	outFile.close();
	std::ofstream outFile1(cur_dir/"constant/options/gases");
	for (i=0;i<ph_nsolu;i++) // solu number
		{ 
		Cgtot = 0;
		for (j=0;j<ph_gcomp;j++) // loop component to calculate Cgtot
			{ Cgtot += freak.g[j*ph_nsolu+i];}
		for (j=0;j<ph_gcomp;j++) // loop component to to writeCgi		
			{ float a = freak.g[j*ph_nsolu+i]/Cgtot; outFile1 << a << "\n"; } //in fraction /Cgtot/phreeqcVm*1000
		}
	outFile1.close();
	
	//##################### make the initialization for the full domain in phreeqc : data,poro, gvol
	std::ifstream inputData{cur_dir/"phqfoam.txt"};
	std::vector<int> ph_data{std::istream_iterator<int>{inputData}, {}}; //for (int i=0; i<7;i++){Info << "init nb "<< ph_data[i] << endl;}
	nxyz=ph_data[0];ph_ncomp=ph_data[1];ph_gcomp=ph_data[2];ph_nsolu=ph_data[3]; //!!! nxyz here is inside the ractive part
	freak.setData(ph_data); Info << "nxyz " << nxyz << endl;
	//initiate poro and gas volume
	poro.resize(nxyz,0);Info<<"poro size "<<poro.size()<<endl;
	for (i=0;i<nxyz;i++) {poro[i]=eps[i];}
	freak.setPoro(poro); Info << "poro 10 350  " << poro[10]<< " "<<poro[350] << endl;
	gvol.resize(nxyz,0);
	for (i=0;i<nxyz;i++) {gvol[i] = max(eps[i]*(1.-sw[i]),1e-4);}
	freak.setGvol(gvol);  Info << "gvol 0 1  " << gvol[0]<< " "<<gvol[1] << endl; // set gas volume in phreqc
	// trial to run a first phreeqc
	freak.init(); 
	std::vector<double> wsat(nxyz,1.-1e-4);freak.setWsat(wsat);
	Info<<"frkg size "<<freak.g.size() <<"g 0 0 "<<freak.g[0]<<endl;
	gm_ph.resize(nxyz*ph_gcomp,0);
	for (i=0; i<ph_gcomp;i++)
		for (int j=0;j<nxyz;j++) {gm_ph[i*nxyz+j] = freak.g[i*nxyz+j]/(p[j]/atmPa)*gvol[j]/phreeqcVm;
				if (j==0) {Info<<"gm_ph i "<<i<<" j "<<j<<" frk "<< freak.g[i*nxyz+j]<<" "<<gm_ph[i*nxyz+j]<<endl;} }
	freak.setGm(gm_ph);
	freak.run();
	
	#include "createFvOptions.H"

	//##############" build the c_ph and gm_ph fields and get conc from phreeqc (c_ph=freak.c but needed for format questions)
	c_ph.resize(nxyz*ph_ncomp,0);
	for (i=0; i<ph_ncomp;i++)
		for (int j=0;j<nxyz;j++) 
			{c_ph[i*nxyz+j] = freak.c[i*nxyz+j];}
	gm_ph.resize(nxyz*ph_gcomp,0);
	// gases are in bars (freak.g)
	Info<<"gcomp "<<ph_gcomp<<" g size "<<freak.g.size()<<endl;
	for (i=0; i<ph_gcomp;i++)
		for (int j=0;j<nxyz;j++) 
			{gm_ph[i*nxyz+j] = freak.g[i*nxyz+j]/(p[j]/atmPa)*gvol[j]/phreeqcVm;}
	iw = freak.iGwater;
	
	//################### build the Cwi and Cgi fields and attribute them concentrations/pressures
	#include "transport/createCwiFields.H"
	#include "transport/createCgiFields.H"
	int icnt = 0;
	forAll(Cw,i) {
		for (j=0; j<nxyz;j++){Cw[i]()[ractive[j]] = c_ph[i*nxyz+j];} // transfer freak.c to Cw
		} 
	// here the freak.g transmit pressures in bars
	icnt = 0;
	for (j=0; j<nxyz;j++)
		{
		Cgtot = 0;
		forAll(Cg,i){Cgtot += freak.g[i*nxyz+j];} 
		forAll(Cg,i){Cg[i]()[j] = freak.g[i*nxyz+j]/Cgtot;} // /Cgtot/phreeqcVm*1000;}
		} 	
	Info<<" cg 0 0 "<<Cg[0]()[0]<<" cg 1 0 "<<Cg[1]()[0]<<" cg 0 1 "<<Cg[0]()[1]<<endl;
	
	dimensionedScalar sT = runTime.startTime();
	dimensionedScalar eT = runTime.endTime();
	dimensionedScalar dT = mesh.time().deltaTValue();
	scalar dT1 = runTime.controlDict().lookupOrDefault("writeInterval",0)/10;Info<<"dt1 "<<dT1<<endl;
	
	if (flowStartSteady==1.)
		{
		while (simple.loop(runTime))
			{
			#include "hstdEqn.H"
			}
		}
	runTime.setTime(sT,0); // 12/3/21 time value and index
	runTime.setEndTime(eT); // 12/3/21 set end time that was lost during the simple loop in hstdEqn
	runTime.runTimeModifiable();
	runTime.read();
	runTime.setDeltaT(dT);
	float oldTime=0;float a=0;
	// to set pressure to 1b and molar volume for case without p calculation (no temp depednence up to now)
	//if (activate2phase == 0) {phreeqcVm = 24.5;}
	
	int istep = 0;int tcnt = 0;
	while (runTime.run())
    {
		runTime.read();
		#include "setDeltaT.H"
		runTime++;
		Info << "time = " << runTime.timeName() <<  "  deltaT = " <<  runTime.deltaTValue() << endl;
		// *********** here provide change of density and viscosity if required
		
		//***********************  solve transient flow   *******************************
		//for (j=0; j<nxyz;j++) {if (j<6) {Info<<"p before flow "<<p[j]/atmPa<<" sw "<<sw[j]<<endl;}}
		#include "flow.H"
		for (j=0; j<nxyz;j++) {gvol[j] = max(eps[j]*(1.-sw[j]),1e-5);
					if (j==2) {Info<<"eps 2 "<<eps[j]<<" sw "<<sw[j]<<" gvol "<<gvol[j]<<endl;}}
		freak.setGvol(gvol); // set gas volume in phreeqc
		
		//***************  solve Transport  *************************
		forAll(Cw,i) {Cw[i]().storePrevIter();} // for cells outside calculation
		forAll(Cg,i) {Cg[i]().storePrevIter();}
		//modify the partial pressure to consider p change (dualphase)
		#include "transport/CwiEqn.H"
		#include "transport/CgiEqn.H"
			Info<<" cg 0 0 "<<Cg[0]()[0]<<" cg 1 0 "<<Cg[1]()[0]<<" cg 0 1 "<<Cg[0]()[1]<<endl;
		//for (i=0; i<ph_ncomp;i++){Info << i <<" C 1st, 118, 1732 "<< Cw[i]()[0] << "  " << Cw[i]()[118] << " " << Cw[i]()[1732] << endl;}
		
		//***************  solve reaction  *************************
		// find where the transported conc have changed to calculate only there
		//Info<<"runtime "<<runTime.value()-oldTime<<endl;
		tcnt++;
		if (tcnt>reactionSteps-1) {tcnt=0;}
		if (activateReaction==1 && tcnt==0)  //runTime.value()-oldTime>dT1
		{
			// find the cells where the chcemistry has changed to calcualte there
			icnt = 0;
			std::vector<double> rchange(nxyz,0.);Info <<"ractive size "<<ractive.size()<<endl;
			for (i=4; i<ph_ncomp; i++)
				{
				for (j=0; j<nxyz;j++)
					{
					if (abs(c_ph[i*nxyz+j]-Cw[i]()[ractive[j]])/(c_ph[i*nxyz+j]+1e-20)>1e-5 && sw[ractive[j]]>0.001) {rchange[j] = max(rchange[j],1.);}
					}
				} 
			for (i=0; i<ph_gcomp; i++)
				{
				for (j=0; j<nxyz;j++)
					{
					if (abs(gm_ph[i*nxyz+j]-Cg[i]()[ractive[j]])/(gm_ph[i*nxyz+j]+1e-20)>1e-5 && sw[ractive[j]]>0.001) {rchange[j] = max(rchange[j],1.);}
					}
				} 
			
			// transfer to phreeqc for c and g, for g we send moles and not pressures!!!
			int icnt = 0;
			for (j=0; j<nxyz;j++) { forAll(Cw,i) {c_ph[i*nxyz+j] = Cw[i]()[ractive[j]];} } 
			if (activate2phase == 1)
				{
				for (j=0; j<nxyz;j++) { 
					forAll(Cg,i) {
					gm_ph[i*nxyz+j] = max(Cg[i]()[j]*gvol[j]/phreeqcVm,1e-9);} //Cg in fraction and Vm in L/mol
					} 
				}
			else 
				{
					forAll(Cg,i) { for (j=0; j<nxyz;j++) {gm_ph[i*nxyz+j] = Cg[i]()[j]*gvol[j]/phreeqcVm; } } // Cg mol/m3 and gvol in L/dm3
				}
			Info<<" cg 0 0 "<<Cg[0]()[0]<<" cg 1 0 "<<Cg[1]()[0]<<" cg 0 1 "<<Cg[0]()[1]<<endl;
			
			// set saturations using rchange
			for (j=0; j<nxyz;j++) {rchange[j] = sw[ractive[j]];}//Info<<"rch "<<rchange[j]<<endl;}
			//Info<<" sw 2 "<<sw[2]<<" "<<endl;
			
			//auto start = std::chrono::high_resolution_clock::now();
			//################# RUN PHREEQC   ################
				freak.setC(c_ph);//transfer c_ph to freak : it does not work to send directly to freak.c
				freak.setGm(gm_ph);//transfer gm_ph to freak
				//freak.setP(pback);//transfer pressure to freak
				freak.setWsat(rchange); // rchange for the calculation doamin, with 0 outside, sw saturation
				freak.setTstep(runTime.value()-oldTime); //Info<<" this tme "<< runTime.value()<<" old "<<oldTime<<endl;//the calculation time shall include all time since las phreeqc run
				Info << "running phreeqc "<<endl;
				freak.run();
				freak.getSelOutput();
				Info << "phreeqc done "<<endl;
			
			//auto finish = std::chrono::high_resolution_clock::now();
			//std::chrono::duration<double> dt = finish - start;dure = dure+dt.count();
			
			// transfer back to C but before keep the previous values for outside domain of calculation
			forAll(Cw,i) {Cw[i]() = Cw[i]().prevIter();}
			forAll(Cg,i) {Cg[i]() = Cg[i]().prevIter();}
			forAll(Cw,i) // dissolved
				{
					for (j=0; j<nxyz;j++)
						{
						Cw[i]()[ractive[j]] = freak.c[i*nxyz+j];
						if (j==0) {Info<<"i "<<i<<" j "<<j<<" c "<<Cw[i]()[j]<<endl;}
						}
				} 
			
			// gas, read partial pressures (freak.g in atm) set Cg to fraction (freak.g/Cgtot) and set p to sum of Cg
			if (activate2phase == 1) 
				{
				for (j=0; j<nxyz;j++) //should consider ractive
					{
					Cgtot = 0;
					forAll(Cg,i) {freak.g[i*nxyz+j] = max(freak.g[i*nxyz+j],0.); Cgtot += freak.g[i*nxyz+j];}
					forAll(Cg,i) {Cg[i]()[j] = freak.g[i*nxyz+j]/Cgtot;} // /Cgtot/phreeqcVm*1000;}
					p[j] = Cgtot*atmPa;
					}
				for (int i=0; i<ph_gcomp;i++){for (j=0;j<3;j++) {Info <<"g_spc "<< i <<" Cg "<< Cg[i]()[j] <<" sw "<<sw[j]<< endl;}}
				}
			else  
				{
				for (j=0; j<nxyz;j++)
					{
					Cgtot = 0;
					forAll(Cg,i){Cgtot += freak.g[i*nxyz+j];} 
					forAll(Cg,i){Cg[i]()[j] = freak.g[i*nxyz+j]/Cgtot;} //1000
					} 	
					//forAll(Cg,i) {  {Cg[i]()[j] = max(freak.g[i*nxyz+j]/Cgtot[j]/phreeqcVm*1000,1e-8); } }
				}
			// find the variation of wsat from nb moles H2O in gas phase
			if (freak.iGwater>-1) //should consider ractive
				{
				//iw = freak.iGwater; done at start
				for (j=0; j<nxyz;j++)
					{
					a = max(freak.gm[iw*nxyz+j],0.) - gm_ph[iw*nxyz+j]; // delta moles of water as gas
					if (j<5) {Info<< " gm_ph "<< gm_ph[iw*nxyz+j] << " frk "<< freak.gm[iw*nxyz+j] <<" a "<<a<< endl;}
					sw[j] = max(0.01,sw[j] - a*.08101/eps[j]);
					}
				} 
				//nb of moles of H2O(g) transformed in water volume (1 mol 18.01 mL at 25°C)
			for (j=0;j<3;j++) {Info <<" new sw "<<sw[j]<< endl;}
				
			oldTime = runTime.value();
		}
		
		bool ts;
		ts = runTime.write();//oldTime=runTime.value();
		//write species
		if (ts ) {
			phiw.write();
			std::ofstream outFile(cur_dir/ runTime.timeName() /"Species");
			//outFile.setf(ios::fixed);
			outFile.unsetf(std::ios::scientific);outFile.precision(6);
			for (const auto &x : freak.spc) outFile << x << "\n";
		}
		
		Info << "ExecutionTime = " << runTime.elapsedCpuTime() << " s"
			<< "  ClockTime = " << runTime.elapsedClockTime() << " s"
			<< nl << endl;

		Info<< "End\n" << endl;
		//Info <<"phreeqc time" << dure << endl;
		istep ++;
	}
    return 0;
}

// ************************************************************************* //
