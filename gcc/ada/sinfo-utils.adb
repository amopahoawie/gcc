------------------------------------------------------------------------------
--                                                                          --
--                         GNAT COMPILER COMPONENTS                         --
--                                                                          --
--                           S I N F O . U T I L S                          --
--                                                                          --
--                                 B o d y                                  --
--                                                                          --
--           Copyright (C) 2020-2025, Free Software Foundation, Inc.        --
--                                                                          --
-- GNAT is free software;  you can  redistribute it  and/or modify it under --
-- terms of the  GNU General Public License as published  by the Free Soft- --
-- ware  Foundation;  either version 3,  or (at your option) any later ver- --
-- sion.  GNAT is distributed in the hope that it will be useful, but WITH- --
-- OUT ANY WARRANTY;  without even the  implied warranty of MERCHANTABILITY --
-- or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License --
-- for  more details.  You should have  received  a copy of the GNU General --
-- Public License  distributed with GNAT; see file COPYING3.  If not, go to --
-- http://www.gnu.org/licenses for a complete copy of the license.          --
--                                                                          --
-- GNAT was originally developed  by the GNAT team at  New York University. --
-- Extensive contributions were provided by Ada Core Technologies Inc.      --
--                                                                          --
------------------------------------------------------------------------------

with Atree;  use Atree;
with Debug;  use Debug;
with GNAT.Lists;
with Output; use Output;
with Seinfo;
with Sinput; use Sinput;

package body Sinfo.Utils is

   function Spec_Lib_Unit
     (N : N_Compilation_Unit_Id) return Opt_N_Compilation_Unit_Id
   is
      pragma Assert (Unit (N) in N_Lib_Unit_Body_Id);
   begin
      return Val : constant Opt_N_Compilation_Unit_Id :=
        Spec_Or_Body_Lib_Unit (N)
      do
         pragma Assert
           (if Present (Val) then
             Unit (Val) in N_Lib_Unit_Declaration_Id
              | N_Lib_Unit_Renaming_Declaration_Id -- only in case of error
             or else (N = Val
                      and then Unit (N) in N_Subprogram_Body_Id
                      and then Acts_As_Spec (N)));
      end return;
   end Spec_Lib_Unit;

   procedure Set_Spec_Lib_Unit (N, Val : N_Compilation_Unit_Id) is
      pragma Assert (Unit (N) in N_Lib_Unit_Body_Id);
      pragma Assert
        (Unit (Val) in N_Lib_Unit_Declaration_Id
          | N_Lib_Unit_Renaming_Declaration_Id -- only in case of error
         or else (N = Val
                  and then Unit (N) in N_Subprogram_Body_Id
                  and then Acts_As_Spec (N)));
   begin
      Set_Library_Unit (N, Val);
   end Set_Spec_Lib_Unit;

   function Body_Lib_Unit
     (N : N_Compilation_Unit_Id) return Opt_N_Compilation_Unit_Id
   is
      pragma Assert
        (Unit (N) in N_Lib_Unit_Declaration_Id
          | N_Lib_Unit_Renaming_Declaration_Id); -- only in case of error
   begin
      return Val : constant Opt_N_Compilation_Unit_Id :=
        Spec_Or_Body_Lib_Unit (N)
      do
         pragma Assert
           (if Present (Val) then Unit (Val) in N_Lib_Unit_Body_Id);
      end return;
   end Body_Lib_Unit;

   procedure Set_Body_Lib_Unit (N, Val : N_Compilation_Unit_Id) is
      pragma Assert
        (Unit (N) in N_Lib_Unit_Declaration_Id
          | N_Lib_Unit_Renaming_Declaration_Id); -- only in case of error
      pragma Assert (Unit (Val) in N_Lib_Unit_Body_Id);
   begin
      Set_Library_Unit (N, Val);
   end Set_Body_Lib_Unit;

   function Spec_Or_Body_Lib_Unit
     (N : N_Compilation_Unit_Id) return Opt_N_Compilation_Unit_Id
   is
      pragma Assert
        (Unit (N) in
          N_Lib_Unit_Declaration_Id | N_Lib_Unit_Body_Id
          | N_Lib_Unit_Renaming_Declaration_Id);
   begin
      return Other_Comp_Unit (N);
   end Spec_Or_Body_Lib_Unit;

   function Subunit_Parent
     (N : N_Compilation_Unit_Id) return Opt_N_Compilation_Unit_Id
   is
      pragma Assert (Unit (N) in N_Subunit_Id);
   begin
      return Val : constant Opt_N_Compilation_Unit_Id := Other_Comp_Unit (N) do
         pragma Assert
           (if Present (Val) then
             Unit (Val) in N_Lib_Unit_Body_Id | N_Subunit_Id);
      end return;
   end Subunit_Parent;

   procedure Set_Subunit_Parent (N, Val : N_Compilation_Unit_Id) is
      pragma Assert (Unit (N) in N_Subunit_Id);
      pragma Assert (Unit (Val) in N_Lib_Unit_Body_Id | N_Subunit_Id);
   begin
      Set_Library_Unit (N, Val);
   end Set_Subunit_Parent;

   function Other_Comp_Unit
     (N : N_Compilation_Unit_Id) return Opt_N_Compilation_Unit_Id
   is
      pragma Assert (N in N_Compilation_Unit_Id);
      Val : constant Opt_N_Compilation_Unit_Id := Library_Unit (N);
   begin
      if Unit (N) in N_Subunit_Id then
         pragma Assert
           (if Present (Val) then
             Unit (Val) in N_Lib_Unit_Body_Id | N_Subunit_Id);
      end if;

      return Library_Unit (N);
   end Other_Comp_Unit;

   function Stub_Subunit
     (N : N_Body_Stub_Id) return Opt_N_Compilation_Unit_Id is
   begin
      return Val : constant Opt_N_Compilation_Unit_Id := Library_Unit (N) do
         pragma Assert (if Present (Val) then Unit (Val) in N_Subunit_Id);
      end return;
   end Stub_Subunit;

   procedure Set_Stub_Subunit
     (N : N_Body_Stub_Id; Val : N_Compilation_Unit_Id)
   is
      pragma Assert (Unit (Val) in N_Subunit_Id);
   begin
      Set_Library_Unit (N, Val);
   end Set_Stub_Subunit;

   function Withed_Lib_Unit
     (N : N_With_Clause_Id) return Opt_N_Compilation_Unit_Id is
   begin
      return Val : constant Opt_N_Compilation_Unit_Id := Library_Unit (N) do
         pragma Assert
           (if Present (Val) then
             Unit (Val) in N_Lib_Unit_Declaration_Id
             | N_Lib_Unit_Renaming_Declaration_Id
             | N_Package_Body_Id | N_Subprogram_Body_Id
             | N_Null_Statement_Id); -- for ignored ghost code
      end return;
   end Withed_Lib_Unit;

   procedure Set_Withed_Lib_Unit
     (N : N_With_Clause_Id; Val : N_Compilation_Unit_Id)
   is
      pragma Assert
        (Unit (Val) in N_Lib_Unit_Declaration_Id
         | N_Lib_Unit_Renaming_Declaration_Id
         | N_Package_Body_Id | N_Subprogram_Body_Id);
   begin
      Set_Library_Unit (N, Val);
   end Set_Withed_Lib_Unit;

   ---------------
   -- Debugging --
   ---------------

   --  Suppose you find that node 12345 is messed up. You might want to find
   --  the code that created that node. There are two ways to do this:

   --  One way is to set a conditional breakpoint on New_Node_Debugging_Output
   --  (nickname "nnd"):
   --     break nnd if n = 12345
   --  and run gnat1 again from the beginning.

   --  The other way is to set a breakpoint near the beginning (e.g. on
   --  gnat1drv), and run. Then set Watch_Node (nickname "ww") to 12345 in gdb:
   --     ww := 12345
   --  and set a breakpoint on New_Node_Breakpoint (nickname "nn"). Continue.

   --  Either way, gnat1 will stop when node 12345 is created, or certain other
   --  interesting operations are performed, such as Rewrite. To see exactly
   --  which operations, search for "New_Node_Debugging_Output" in Atree.

   --  The second method is much faster if the amount of Ada code being
   --  compiled is large.

   ww : Node_Id'Base := Node_Low_Bound - 1;
   pragma Export (Ada, ww);
   Watch_Node : Node_Id'Base renames ww;
   --  Node to "watch"; that is, whenever a node is created, we check if it
   --  is equal to Watch_Node, and if so, call New_Node_Breakpoint. You have
   --  presumably set a breakpoint on New_Node_Breakpoint. Note that the
   --  initial value of Node_Id'First - 1 ensures that by default, no node
   --  will be equal to Watch_Node.

   procedure nn;
   pragma Export (Ada, nn);
   procedure New_Node_Breakpoint renames nn;
   --  This doesn't do anything interesting; it's just for setting breakpoint
   --  on as explained above.

   procedure nnd (N : Node_Id);
   pragma Export (Ada, nnd);
   --  For debugging. If debugging is turned on, New_Node and New_Entity (etc.)
   --  call this. If debug flag N is turned on, this prints out the new node.
   --
   --  If Node = Watch_Node, this prints out the new node and calls
   --  New_Node_Breakpoint. Otherwise, does nothing.

   procedure Node_Debug_Output (Op : String; N : Node_Id);
   --  Called by nnd; writes Op followed by information about N

   -------------------------
   -- New_Node_Breakpoint --
   -------------------------

   procedure nn is
   begin
      Write_Str ("Watched node ");
      Write_Int (Int (Watch_Node));
      Write_Eol;
   end nn;

   -------------------------------
   -- New_Node_Debugging_Output --
   -------------------------------

   procedure nnd (N : Node_Id) is
      Node_Is_Watched : constant Boolean := N = Watch_Node;

   begin
      if Debug_Flag_N or else Node_Is_Watched then
         Node_Debug_Output ("Node", N);

         if Node_Is_Watched then
            New_Node_Breakpoint;
         end if;
      end if;
   end nnd;

   procedure New_Node_Debugging_Output (N : Node_Id) is
   begin
      pragma Debug (nnd (N));
   end New_Node_Debugging_Output;

   -----------------------
   -- Node_Debug_Output --
   -----------------------

   procedure Node_Debug_Output (Op : String; N : Node_Id) is
   begin
      Write_Str (Op);

      if Nkind (N) in N_Entity then
         Write_Str (" entity");
      else
         Write_Str (" node");
      end if;

      Write_Str (" Id = ");
      Write_Int (Int (N));
      Write_Str ("  ");
      Write_Location (Sloc (N));
      Write_Str ("  ");
      Write_Str (Node_Kind'Image (Nkind (N)));
      Write_Eol;
   end Node_Debug_Output;

   -------------------------------
   -- Parent-related operations --
   -------------------------------

   procedure Copy_Parent (To, From : Node_Or_Entity_Id) is
   begin
      if Atree.Present (To) and Atree.Present (From) then
         Atree.Set_Parent (To, Atree.Parent (From));
      else
         pragma Assert
           (if Atree.Present (To) then Atree.No (Atree.Parent (To)));
      end if;
   end Copy_Parent;

   function Parent_Kind (N : Node_Id) return Node_Kind is
   begin
      if Atree.No (N) then
         return N_Empty;
      else
         return Nkind (Atree.Parent (N));
      end if;
   end Parent_Kind;

   -------------------------
   -- Iterator Procedures --
   -------------------------

   procedure Next_Entity       (N : in out Node_Id) is
   begin
      N := Next_Entity (N);
   end Next_Entity;

   procedure Next_Named_Actual (N : in out Node_Id) is
   begin
      N := Next_Named_Actual (N);
   end Next_Named_Actual;

   procedure Next_Rep_Item     (N : in out Node_Id) is
   begin
      N := Next_Rep_Item (N);
   end Next_Rep_Item;

   procedure Next_Use_Clause   (N : in out Node_Id) is
   begin
      N := Next_Use_Clause (N);
   end Next_Use_Clause;

   ------------------
   -- End_Location --
   ------------------

   function End_Location (N : Node_Id) return Source_Ptr is
      L : constant Valid_Uint := End_Span (N);
   begin
      return Sloc (N) + Source_Ptr (UI_To_Int (L));
   end End_Location;

   --------------------
   -- Get_Pragma_Arg --
   --------------------

   function Get_Pragma_Arg (Arg : Node_Id) return Node_Id is
   begin
      if Nkind (Arg) = N_Pragma_Argument_Association then
         return Expression (Arg);
      else
         return Arg;
      end if;
   end Get_Pragma_Arg;

   -----------------------
   -- Loop_Flow_Keyword --
   -----------------------

   function Loop_Flow_Keyword (N : N_Loop_Flow_Statement_Id) return String is
   begin
      case Nkind (N) is
         when N_Continue_Statement => return "continue";
         when N_Exit_Statement => return "exit";
         when others => pragma Assert (False);
      end case;
   end Loop_Flow_Keyword;

   procedure Destroy_Element (Elem : in out Union_Id);
   --  Does not do anything but is used to instantiate
   --  GNAT.Lists.Doubly_Linked_Lists.

   ---------------------
   -- Destroy_Element --
   ---------------------

   procedure Destroy_Element (Elem : in out Union_Id) is
   begin
      null;
   end Destroy_Element;

   package Lists is
     new GNAT.Lists.Doubly_Linked_Lists
       (Element_Type => Union_Id, "=" => "=",
      Destroy_Element => Destroy_Element, Check_Tampering => False);

   ----------------------------
   -- Lowest_Common_Ancestor --
   ----------------------------

   function Lowest_Common_Ancestor (N1, N2 : Node_Id) return Union_Id is
      function Path_From_Root (N : Node_Id) return Lists.Doubly_Linked_List;

      --------------------
      -- Path_From_Root --
      --------------------

      function Path_From_Root (N : Node_Id) return Lists.Doubly_Linked_List is
         L : constant Lists.Doubly_Linked_List := Lists.Create;

         X : Union_Id := Union_Id (N);
      begin
         while X /= Union_Id (Empty) loop
            Lists.Prepend (L, X);
            X := Parent_Or_List_Containing (X);
         end loop;

         return L;
      end Path_From_Root;

      L1 : Lists.Doubly_Linked_List := Path_From_Root (N1);
      L2 : Lists.Doubly_Linked_List := Path_From_Root (N2);

      X1, X2 : Union_Id;

      Common_Ancestor : Union_Id := Union_Id (Empty);
   begin
      while not Lists.Is_Empty (L1) and then not Lists.Is_Empty (L2) loop
         X1 := Lists.First (L1);
         Lists.Delete_First (L1);

         X2 := Lists.First (L2);
         Lists.Delete_First (L2);

         exit when X1 /= X2;

         Common_Ancestor := X1;
      end loop;

      Lists.Destroy (L1);
      Lists.Destroy (L2);

      return Common_Ancestor;
   end Lowest_Common_Ancestor;

   ----------------------
   -- Set_End_Location --
   ----------------------

   procedure Set_End_Location (N : Node_Id; S : Source_Ptr) is
   begin
      Set_End_Span (N,
        UI_From_Int (Int (S - Sloc (N))));
   end Set_End_Location;

   --------------------------
   -- Pragma_Name_Unmapped --
   --------------------------

   function Pragma_Name_Unmapped (N : Node_Id) return Name_Id is
   begin
      return Chars (Pragma_Identifier (N));
   end Pragma_Name_Unmapped;

   ------------------------------------
   -- Helpers for Walk_Sinfo_Fields* --
   ------------------------------------

   function Get_Node_Field_Union is new
     Atree.Atree_Private_Part.Get_32_Bit_Field (Union_Id) with Inline;
   procedure Set_Node_Field_Union is new
     Atree.Atree_Private_Part.Set_32_Bit_Field (Union_Id) with Inline;

   use Seinfo;

   function Is_In_Union_Id (F_Kind : Field_Kind) return Boolean is
   --  True if the field type is one that can be converted to Types.Union_Id
     (case F_Kind is
       when Node_Id_Field
          | List_Id_Field
          | Elist_Id_Field
          | Name_Id_Field
          | String_Id_Field
          | Valid_Uint_Field
          | Unat_Field
          | Upos_Field
          | Nonzero_Uint_Field
          | Uint_Field
          | Ureal_Field
          | Union_Id_Field => True,
       when Flag_Field
          | Node_Kind_Type_Field
          | Entity_Kind_Type_Field
          | Source_File_Index_Field
          | Source_Ptr_Field
          | Small_Paren_Count_Type_Field
          | Convention_Id_Field
          | Component_Alignment_Kind_Field
          | Mechanism_Type_Field => False);

   -----------------------
   -- Walk_Sinfo_Fields --
   -----------------------

   procedure Walk_Sinfo_Fields (N : Node_Id) is
      Fields : Node_Field_Array renames
        Node_Field_Table (Nkind (N)).all;

   begin
      for J in Fields'Range loop
         if Fields (J) /= F_Link then -- Don't walk Parent!
            declare
               Desc : Field_Descriptor renames
                 Field_Descriptors (Fields (J));
               pragma Assert (Desc.Type_Only = No_Type_Only);
               --  Type_Only is for entities
            begin
               if Is_In_Union_Id (Desc.Kind) then
                  Action (Get_Node_Field_Union (N, Desc.Offset));
               end if;
            end;
         end if;
      end loop;
   end Walk_Sinfo_Fields;

   --------------------------------
   -- Walk_Sinfo_Fields_Pairwise --
   --------------------------------

   procedure Walk_Sinfo_Fields_Pairwise (N1, N2 : Node_Id) is
      pragma Assert (Nkind (N1) = Nkind (N2));

      Fields : Node_Field_Array renames
        Node_Field_Table (Nkind (N1)).all;

   begin
      for J in Fields'Range loop
         if Fields (J) /= F_Link then -- Don't walk Parent!
            declare
               Desc : Field_Descriptor renames
                 Field_Descriptors (Fields (J));
               pragma Assert (Desc.Type_Only = No_Type_Only);
               --  Type_Only is for entities
            begin
               if Is_In_Union_Id (Desc.Kind) then
                  Set_Node_Field_Union
                    (N1, Desc.Offset,
                     Transform (Get_Node_Field_Union (N2, Desc.Offset)));
               end if;
            end;
         end if;
      end loop;
   end Walk_Sinfo_Fields_Pairwise;

   ---------------------
   -- Map_Pragma_Name --
   ---------------------

   --  We don't want to introduce a dependence on some hash table package or
   --  similar, so we use a simple array of Key => Value pairs, and do a linear
   --  search. Linear search is plenty efficient, given that we don't expect
   --  more than a couple of entries in the mapping.

   type Name_Pair is record
      Key   : Name_Id;
      Value : Name_Id;
   end record;

   type Pragma_Map_Index is range 1 .. 100;
   Pragma_Map : array (Pragma_Map_Index) of Name_Pair;
   Last_Pair : Pragma_Map_Index'Base range 0 .. Pragma_Map_Index'Last := 0;

   procedure Map_Pragma_Name (From, To : Name_Id) is
   begin
      if Last_Pair = Pragma_Map'Last then
         raise Too_Many_Pragma_Mappings;
      end if;

      Last_Pair := Last_Pair + 1;
      Pragma_Map (Last_Pair) := (Key => From, Value => To);
   end Map_Pragma_Name;

   -----------------
   -- Pragma_Name --
   -----------------

   function Pragma_Name (N : Node_Id) return Name_Id is
      Result : constant Name_Id := Pragma_Name_Unmapped (N);
   begin
      for J in Pragma_Map'First .. Last_Pair loop
         if Result = Pragma_Map (J).Key then
            return Pragma_Map (J).Value;
         end if;
      end loop;

      return Result;
   end Pragma_Name;

end Sinfo.Utils;
