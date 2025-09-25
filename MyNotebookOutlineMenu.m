BeginPackage["MyNotebookOutlineMenu`"]

MyNotebookOutlineMenu::usage =
        "MyNotebookOutlineMenu overrides NotebookOutlineMenu from the Wolfram 
	Function Repository.
	Specifically the background color of each menu item is corrected to 
	transparent rather than the incorrect white used by the Repository version."

MyNotebookUISetup::usage = 
	"MyNotebookUISetup sets up my standard notebook UI which includes
	- a checkbox to hide or show Input cells
	- a popup outline menu, listing all sections of the notebook."

(* ==================================================================================================== *)

Begin["`Private`"]

MyNotebookOutlineMenu // ClearAll;
MyNotebookOutlineMenu // Attributes = { HoldFirst };

MyNotebookOutlineMenu[ opts: OptionsPattern[ ActionMenu ] ] :=
    MyNotebookOutlineMenu[ InputNotebook[ ], opts ];

MyNotebookOutlineMenu[ nbo_, opts: OptionsPattern[ ActionMenu ] ] :=
    MyNotebookOutlineMenu[ nbo, "Notebook Outline", opts ];

MyNotebookOutlineMenu[ nbo_, label_, opts: OptionsPattern[ ActionMenu ] ] := 
    MyNotebookOutlineMenu[ nbo, label, Automatic, opts ];

MyNotebookOutlineMenu[ nbo_, label_, Automatic, opts: OptionsPattern[ ActionMenu ] ] := 
    MyNotebookOutlineMenu[
        nbo,
        label,
        Alternatives[
            "Title",
            "Subtitle",
            "Chapter",
            "Section",
            "Subsection",
            "Subsubsection"
        ],
        opts
    ];

MyNotebookOutlineMenu[ nbo_, label_, cellStyles_, opts: OptionsPattern[ ActionMenu ] ] := 
    DynamicModule[ { toOffset, makeActionMenuList, updateMenu, menu },
        
        toOffset[ { { n_? NumberQ, _ }, _ } ] := n;
        toOffset[ { n_? NumberQ, _ } ] := n;
        toOffset[ n_? NumberQ ] := n;
        toOffset[ ___ ] := Inherited;
        
        makeActionMenuList[ cells_ ] := 
            Module[ { offsets, offsetNums, max, min, rng, scaled, spacings, bg, styled },
                offsets = Subtract[
                    toOffset /@ CurrentValue[ cells, CellMargins ],
                    toOffset /@ CurrentValue[ cells, FontSize ]
                ];
                offsetNums = Select[ offsets, NumberQ ];
                max = Max[ offsetNums, 0 ];
                min = Min[ offsetNums, 100 ];
                rng = Replace[ max - min, Except[ _? Positive ] -> 1 ];
                scaled = Replace[ offsets, { n_? NumberQ :> (n - min)/rng, _ :> 1 }, { 1 } ];
                spacings = scaled /. MapIndexed[ #1 -> First @ #2 - 1 & , Union @ scaled ];
(* bg = CurrentValue[ nbo, Background ]; *)
                bg = Transparent;
                
                styled = Replace[
                    NotebookRead @ cells,
                    {
                        Cell[ a_String, b__String, c___? OptionQ ] :> 
                            Style[
                                a, 
                                b, 
                                FontSize :> CurrentValue[ "ControlsFontSize" ], 
                                c, 
                                Background -> bg 
                            ]
                        ,
                        Cell[ a_, b__String, c___? OptionQ ] :> 
                            RawBoxes @ Cell[ 
                                a, 
                                b, 
                                CellFrame -> None, 
                                FontSize :> CurrentValue[ "ControlsFontSize" ], 
                                c, 
                                Background -> bg 
                            ]
                    },
                    { 1 }
                ];

                Apply[
                    Function[
                        Row[ {
                            StringJoin @ ConstantArray[ " ", 4 * #2 ],
                            #1
                        } ] :> (
                        SelectionMove[
                            Notebooks @ #3,
                            After,
                            Notebook,
                            AutoScroll -> True
                        ];

                        SelectionMove[ #3, All, Cell, AutoScroll -> True ])
                    ],
                    Transpose @ { styled, spacings, cells },
                    { 1 }
                ]
            ];

        
        updateMenu[ ] := 
            If[ FailureQ @ Developer`NotebookInformation @ nbo,
                ActionMenu[ label, { }, Enabled -> False ],
                With[ { cells = Cells[ nbo, CellStyle -> cellStyles ] },
                    ActionMenu[ label, makeActionMenuList @ cells, opts ]
                ]
            ];

        menu = updateMenu[ ];
        EventHandler[ Dynamic @ menu, { "MouseEntered" :> (menu = updateMenu[ ]) } ]
    ];

(* ---------------------------------------------------------------------------------------------------- *)

(*
The important elements below include

- The first block of stuff, the DockedCells stuff, 
creates a checkbox that sits at the top of the notebook,
and that dynamically is linked to the "secret 
variable" called"MyCellOpen".
(We go through this rigamarole with TaggingRules and suchlike so we 
don't create a variable visible to the rest of the system and notebook.)


The second block of stuff describes how we want cells in our notebook 
to be displayed.

We want
- starting point is everything looks like normal
- then we initialize the "secret variable" "MyCellOpen" to TRUE
- then we say that for a special class of cells,
the Input cells,
we want the value of the CellOpen property (ie whether the Notebook 
displays the cell as open or closed) to be equal to the "MyCellOpen" 
variable that's being toggled by the checkbox;
and we tell the system that non-open cells should be fully hidden  wol
(ie allow their height to be zero) rather than just "less visible"
-  finally we make the checkbox controlling all this look a little more 
visible by giving it a blue background.
*)
MyNotebookUISetup[]:=
Module[ {nb = EvaluationNotebook[]},
SetOptions[nb,
 DockedCells -> {
   Cell[BoxData@RowBox[{
       CheckboxBox[
        Dynamic@CurrentValue[nb, {TaggingRules, "MyCellOpen"}]
        ], "Show Input"}],
    TextAlignment -> Right],
   Cell[BoxData@ToBoxes[
      MyNotebookOutlineMenu[nb]
   ]]
   },
 
 StyleDefinitions -> Notebook[{
    Cell[StyleData[StyleDefinitions -> "Default.nb"]],
    Cell[StyleData["Notebook"],
     TaggingRules -> {"MyCellOpen" -> True}],
    Cell[StyleData["Input"],
     CellOpen :> Dynamic@CurrentValue[nb, {TaggingRules, "MyCellOpen"}],
     CellElementSpacings -> {"CellMinHeight" -> 0, 
       "ClosedCellHeight" -> 0}],
    Cell[StyleData["DockedCell"],
     CellFrameMargins -> 0, Background -> LightBlue]}
   ]]
 ]
(* ---------------------------------------------------------------------------------------------------- *)

End[ ]

EndPackage[ ]

(*
SetDirectory[NotebookDirectory[]];
<< "MyNotebookOutlineMenu.m";
MyNotebookUISetup[];
*)